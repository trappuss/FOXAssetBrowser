#pragma once
#include <QMainWindow>

#include "util/ModPackage.h"
#include <QPointer>
#include <QVector>

class FilesTab;
class TexturesTab;
class ModelsTab;
class BulkExtractorTab;
class CustomizeTab;
class QLabel;
class QToolButton;
class QMenu;
class QTabWidget;
namespace fox { class LogConsole; }

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Dev/agent screenshot harness ("--shot"): once the index is ready, open
    // the requested view, settle, save a PNG of the window, and (unless
    // `stay`) quit. Lets an automated session SEE the running UI.
    struct DevShot {
        QString outPng;
        QString modelFilter;
        bool grid = false;     // Models tab: show the thumbnail grid
        int gridIcon = 0;
        bool filterPopup = false;  // Models tab: open the Filter dropdown
        bool showDebugPanel = false;  // Models/Customize: open the material
                                      // debug panel before the grab
        bool showSubmeshTree = false; // Models/Customize: open the submesh
                                      // tree before the grab
        QString hideSubmesh;          // with --submeshes: uncheck the first
                                      // tree row whose label contains this
        // ── The viewport rig, for a headless A/B on the lighting ────────
        // Applied to EVERY GLModelWidget in the window, not to the active
        // tab's: the harness names a view by what it is looking at, and three
        // tabs each own one. Setting all of them is both simpler and right —
        // only one is on screen for the grab.
        QString viewEnv;      // --viewenv <id>: a ViewEnvironment preset id
        QString viewDebug;    // --viewdebug <name>: a DebugView by name
        QString viewLight;    // --light <az,el>: key azimuth/elevation, degrees
        float viewKey = -1.0f;      // --keygain <x>
        float viewAmbient = -1.0f;  // --ambgain <x>
        float viewExposure = -1.0f; // --exposure <x>
        QString viewPanel;    // --viewpanel <graphics|lighting|camera>
        QString shading;      // --shading <wireframe|flat|shaded|rendered>
        QString channel;      // --channel <name of a DebugView>
        QString overlays;     // --overlays <list|all|none>
        QString popover;      // --popover <graphics|lighting|camera>
        QString settingsTab;  // --settings <tab>: open the dialog on that tab
        float viewTurn = 0.0f;  // --turntable <deg/s>: start the auto-orbit
        bool logConsole = false;  // --logconsole: open it before the grab
        QString capturePng;   // --capture <png>: the viewport as an image
        QString captureGif;   // --turngif <gif>: one revolution
        // --animgif <gif>: the LOADED CLIP as a GIF, through the same
        // animation frame provider the Camera page's button uses. Here so
        // that provider is proven end to end rather than assumed: an
        // uninstalled one is a permanently greyed button, which is exactly
        // the state this feature shipped in before.
        QString captureAnimGif;
        int captureFrames = 0;   // --frames <n>: with --turngif
        QString exportParts;  // --exportparts <dir>: one .glb per part
        // --geardump <tsv>: every gear item of every player character, with
        // the id, the in-game name, the model it resolved to and the bone that
        // model roots at. The bridge between what the user SEES in the game
        // and what this tool calls the same thing, which is what a report of
        // "the beret is in the wrong place" needs to be actionable.
        QString gearDump;
        QString matFilter;            // with --matpanel: type this into the
                                      // material panel's filter box
        int pbrOverride = -1;         // -1 leave alone, 0/1 drive the
                                      // viewport's PBR box after the load —
                                      // exercises the LIVE toggle, including
                                      // the reload it triggers when the maps
                                      // were not loaded
        QString dumpTags;          // Models tab: write the tag vocabulary
        QString dumpThumbs;        // "<substring>=<dir>": render each matching
                                   // model to a PNG through ThumbnailRenderer
        QString searchFilter;
        QString exportGlb;
        // --exportanim <glb>: the Models tab's model, RIGGED, with one glTF
        // animation per clip named by --animclips (default: the clip
        // --clip selected). The only headless way to check that an animated
        // export is the motion the viewport plays.
        QString exportAnim;
        // --exportsceneanim <glb>: the Customize scene, rigged, with the
        // clips --animclips names (default: the one --clip selected).
        QString exportSceneAnim;
        // --exportanimdir <dir>: the same selection, one .glb per clip.
        QString exportAnimDir;
        bool strings = false;        // --strings <filter>: the Strings tab…
        QString stringFilter;
        bool stringsAll = false;     // …with "All tables" ticked
        QString stringDump;          // --stringdump <tsv>: every string
        QString animClips;     // "all", or a comma list of substrings/indices
        bool animPanel = false;      // --animpanel [filter]: open the list…
        QString animPanelFilter;     // …with this in its search box, all
                                     // matching rows selected
        QString mtarFilter;    // with modelFilter: load this .mtar…
        QString clipFilter;    // …pick this clip (substring or index)…
        float frame = 0.0f;    // …and pose at this frame before the grab
        QString partsFilter;   // Customize tab: comma-separated part filters
        QString attachCnp;     // with 2+ parts: seat part 1 at part 0's CNP
        QString weaponSpec;    // Customize/Weapon: "receiverFilter[,slot=partFilter…]"
        QString camoFilter;    // with weaponSpec/charSpec: pick this variation
        // Force a .fv2 onto every fitted part by NAME, past the combo. The
        // combo only lists the tables the catalogue ties to the fitted stems,
        // so a table that attaches a model was unreachable from the harness.
        QString fovaForce;
        QString gearColor;     // with weaponSpec/charSpec: pick this gear
                               // colour by swatch stem ("cm_scol4_c17")
        QString presetFilter;  // Customize/Weapon: apply this game build by name
        QString userPreset;    // …or load one of YOUR saved builds by name
        QString savePreset;    // save the built state under this name first
        bool compatOnly = false;   // tick "only parts that fit this weapon"
        QString charSpec;      // Customize/Character: "id[,slot=partFilter…]"
        QString vehicleSpec;   // Customize/Vehicle: "id[,slot=partFilter…]"
        QString texSearch;     // Textures tab: type this and grab
        // The §7 filters, so each one is a measurement rather than a click.
        QString texUsed;       // --texused all|used|orphans
        QString texFormat;     // --texformat dxt1|dxt5|argb|l8
        QString texUserTag;    // --texusertag <tag from the MODEL vocabulary>
        QString texChannel;    // --texchannel rgb|r|g|b|a|luma
        // ── The viewport's keyboard (§5) ────────────────────────────────
        // Every one of these is invisible from a screenshot alone, so each
        // gets a flag: "does double-click pick the right submesh" is a
        // measurement, not something to be satisfied about by looking.
        // ── The N-panel column and the display modes (§4, §6) ───────────
        // A panel that is open, a column that is collapsed and a view mode are
        // all things a screenshot shows and a log does not — and the reverse:
        // "the outliner built 41 folder rows" is a measurement no picture
        // makes. Both halves get a flag.
        QString npanel;        // --npanel <key,key|none>: open exactly these
        QString display;       // --viewmode list|outliner|grid
        QString pickAt;        // --pickat <x,y>: pick there and report the id
        QString rightDrag;     // --rightdrag <x,y,dx,dy>: right-press, drag,
                               // and report whether the menu was swallowed
        QString gizmo;         // --gizmo x|y|z|-x|-y|-z|ortho|hover:<n>
        QString healthAudit;   // --healthaudit <out.tsv>: classify every model
        QString animSort;      // --animsort archive|name|asset|category
        QString outlinerProbe; // --outliner materials,animations[,play]
        QString outlinerDump;  // --outlinerdump out.tsv
        bool    npanelSizes = false;   // --npanelsizes
        int rowZoom = 0;       // --rowzoom <n>: list/outliner font delta
        bool rowZoomSet = false;
        QString selSeq;        // --selseq pick@x,y;ctrl@x,y;…
        QString undoSeq;       // --undoseq field=value;undo;redo;…
        QString variantCensus; // --variantcensus <tsv>
        QString tab;           // --tab <tab bar label>
        bool fileMenu = false; // --filemenu
        bool menuDump = false; // --menudump
        QString partMenu;      // --partmenu x,y
        int selectRows = 0;    // --selectrows <n>
        QString viewKeys;      // --viewkeys <h|shifth|alth|frame|full|help>
        bool viewHelp = false; // --viewhelp: open the F1 list before the grab
        bool overlayMenu = false;  // --overlaymenu: pop the bar's overlay list
        int channelScroll = 0;     // --channelscroll <n>: wheel the caret
        bool channelScrollSet = false;
        QString dumpDfrm;      // .dfrm topology for models matching this
        QString popupType;     // with popupCombo: type this to filter the list
        int hoverRow = -1;     // with popupCombo: rest the pointer on this row
        bool hoverSet = false;
        QString popupCombo;    // open this builder combo before the grab
                               // ("subject", "variant" or a slot name)
        QString dumpTex;       // model-name substring(s): print the archive
                               // codes of every texture those models want
        QString dumpBones;     // model-name substring: list a model's bones
                               // (resolved names + rest translation) — probe
        QString dumpFile;      // "<pathSubstring>=<outPath>": write one indexed
                               // file's bytes to disk and quit — data probe
        QString dumpFova;      // "<pathSubstring>": parse every matching .fv2
                               // and print what it does — substitutions, mesh
                               // groups hidden and shown, models attached and
                               // whether this install carries them
        QString dumpTree;      // "<pathSubstring>=<outDir>": write EVERY indexed
                               // file whose path contains the substring into
                               // outDir, keeping its directory layout — the
                               // bulk form of dumpFile, for when a probe needs
                               // a whole subtree on disk rather than one file
        // ── Bulk Extract, headless (§8) ─────────────────────────────
        // The run machinery — the queue, the manifest, only-new, the failure
        // list, the worker pool, the pause clock and the decode cache — is the
        // largest thing in this tool with no way to exercise it from a script.
        // These flags run it and quit, so "does only-new actually skip" is a
        // measurement rather than a click-through.
        QString bulkOut;       // --bulkout <dir>: run into this folder, quit
        QString bulkQuery;     // --bulk <query>: the filter (may be empty)
        QString bulkExt;       // --bulkext <ext>
        int bulkWorkers = 0;   // --bulkworkers <n>, 0 = auto
        bool bulkOverwrite = false;  // --bulkexisting overwrite
        bool bulkUseQueue = false;   // --bulkqueue: run the saved queue
        bool bulkAll = false;        // --bulkall: yes, really, the whole index
        // --bulkcancel <ms>: press Cancel this long after the run starts.
        // §8 requires a working Cancel and a pause that leaves the ETA alone,
        // and neither had any way to be exercised from a script — so "does
        // cancelling keep the ledger" and "does a cancelled run leave the
        // previous failure list alone" were click-through questions.
        int bulkCancelMs = 0;
        int bulkPauseMs = 0;   // --bulkpause <ms>: pause, then resume 1s later
        // --cachecheck: read one archive entry twice inside a blobcache scope
        // and twice outside it, and report what the cache did. The saving the
        // cache exists for only shows up on an install with packed containers;
        // this proves the MECHANISM on any install, which is what a tree with
        // no packs can still answer.
        bool cacheCheck = false;
        QString bulkQueueAdd;  // --bulkqueueadd: add the filter's matches to
                               // the queue and quit without running
        QString dumpPlayers;   // write the player-character catalogue to this
                               // TSV and quit — data probe
        QString dumpPng;       // "<assetPathNoExt>=<out.png>": decode any one
                               // texture straight to a PNG — the "just let me
                               // look at it" probe
        QString dumpMaterials; // "<modelSubstring>=<outDir>": save every
                               // material's decoded base map as a PNG plus a
                               // TSV of what each one resolved to — the probe
                               // for "which material is this and what is it
                               // actually drawing"
        QString dumpParams;    // "<pathSubstring>=<outTsv>": one row per
                               // NON-texture material parameter — the named
                               // float4s a shader reads, of which
                               // MatParamIndex_0..3 are how a multi-material
                               // surface picks its FMTT presets
        // "<outTsv>": for every .fv2, which models its CONTENTS match — by
        // material-instance hash, by mesh-group hash and by attached model.
        // The wiki says a table is bound to its model by FILENAME and that
        // nothing inside names the target; this measures how far the contents
        // get you instead, which is the question the Camo menu turns on.
        QString dumpFovaBind;
        // "<outTsv>": every clip in every .mtar — where it lives, what it is
        // called, how long it is. The clip NAME is what any grouping or
        // translation has to be built on, so it gets measured before anything
        // is built on top of it.
        QString dumpAnims;
        // "<modelSubstring>=<outTsv>": every motion archive scored against ONE
        // model's skeleton. The probe behind "animations for this model" —
        // the scope filter is only honest if the score separates, so the
        // score is dumped and looked at rather than trusted.
        QString dumpAnimBind;
        // "model" | "all" | "other:<model name>": the animations panel's scope
        // (§4). Applied before --animpanel's filter, so a run can photograph
        // "this model's clips" and count them.
        QString animScope;
        QString dumpFovaCensus; // "<pathSubstring>=<outTsv>": one row per
                               // FOVA substitution across every matching .fv2
                               // — table, material hash, texture role and the
                               // file it binds. The probe that answers "what
                               // does the game actually rebind at runtime",
                               // which is how colour customization works.
        QString dumpPbr;       // "<modelSubstring>": load every matching model
                               // through the FULL PBR path and print which of
                               // the seven maps each material actually got —
                               // the probe that says whether "full PBR" is
                               // loading anything or quietly loading nothing
        QString dumpColors;    // "<modelSubstring>": print the game's colour
                               // palette, and for every matching model how
                               // many of its materials the game would let you
                               // colour — the probe for "what ships white and
                               // what can be painted"
        QString dumpMatCensus; // "<pathSubstring>=<outTsv>": one row per
                               // (model, material, texture role) across EVERY
                               // matching .fmdl — no decode, no render. The
                               // probe that answers "what texture roles does
                               // this game actually use, and how often", which
                               // is what decides what a PBR loader must load.
        QString dumpAvatarTex; // write the avatar texture sets (skin tones,
                               // wrinkles, brows, hair colours, features) to
                               // this TSV and quit — data probe
        QString dumpAvatar;    // write the Survive avatar face-preset table to
                               // this TSV and quit — data probe
        QString dumpHash;      // "<substring>=<outTsv>": every dictionary name
                               // containing the substring, with its 51-bit path
                               // hash — lets an external tool find entries by
                               // name without re-implementing the hash
        QString dumpFiles;     // write every indexed file (path, size, game,
                               // archive) to this TSV and quit — data probe
        QString iconDump;      // --icondump <tsv>: gear + colour icon coverage
        QString lightDump;     // --lightdump <tsv>: the games' own lighting data
        int gearUnlocked = -1; // --unlocked 0|1: the Exclude/Must switch; -1 =
                               // leave whatever the setting says
        QString dumpSwatch;    // decode every Customize/color UI texture into
                               // this directory and list them — data probe
        QString dumpCompat;    // write the per-receiver compatibility survey
                               // to this TSV and quit — data probe
        QString dumpEquip;     // write the EquipCatalog build list to this
        // --camodump <tsv>: every row the Customize weapon camo combo
        // would build, for every weapon in the index, with its section.
        // A combo popup is a top-level window and cannot be photographed.
        QString dumpCamo;
        // --camodefault: the camo default-selection rule, scripted.
        bool camoDefaultTest = false;
        // --restalignsweep <tsv>: every item of every slot the built
        // subject has, posed on the loaded clip, with how far each one
        // landed from the bone it hangs on. Runs AFTER the build and
        // after the clip, because bind pose hides the whole failure.
        QString restAlignSweep;
        // --texusersweep: run the texture->model sweep to COMPLETION and
        // time it. Its cold cost has never been measured because it runs
        // on a worker thread and every harness run so far quit while it
        // was still going, so fox_texusers_v1.bin was never written.
        bool texUserSweep = false;
        // --moddump <tsv>: every replacement in the mod folder, and
        // whether the index actually resolves that asset to it. A
        // replacement that does not WIN is the silent failure of this
        // whole feature, so the check is the point of the dump.
        QString dumpMod;
        // --modpackage <zip>: the mod folder, packaged as one file, with a
        // manifest recording each asset's hash, MD5 and whether the GAME's
        // own copy lives inside a container. The acceptance test is
        // Python's zipfile module reading it back, not this build's.
        QString modPackage;
        // --mgsvpackage <file> [--mgsvmeta "k=v;…"]: the same folder as a
        // real SnakeBite .mgsv. The acceptance test is SnakeBite's own
        // classes, compiled and run against the file this writes.
        QString mgsvPackage;
        QString mgsvMeta;
        // --mgsvdialog: open the metadata form for a --shot.
        bool mgsvDialog = false;
        // --exportslot <slot>=<file>: the part fitted in one slot, alone.
        QString exportSlot;
        // --exportvariations <dir>: one file per row of the variation list.
        QString exportVariations;
        // --slotmenu <slot|row>: the shared slot menu, as text.
        QString slotMenu;
        // --ftexroundtrip <tsv>: every .ftex in the index, assembled to
        // DDS, written back through FtexWriter and assembled again. The
        // two DDS files must be identical. This is the acceptance test
        // for texture replacement and it is free — the extractor that
        // makes the first DDS already existed.
        QString ftexRoundTrip;
        // --modreplacetex <asset>=<dds>: the texture replacement path,
        // end to end — re-encode into the original's layout and install
        // the .ftex and every .N.ftexs as one unit.
        QString modReplaceTex;
        // --texdds <asset>=<file>: assemble one texture to a .dds. The
        // other half of the replace workflow — export, edit, re-import —
        // which the row menu has had all along and the harness had not.
        QString texDds;
        // --assetmenu <asset>: the SHARED row menu for one named asset,
        // logged. --filemenu tests what the Models TAB composes and can
        // only reach a model; this tests exportactions::addFileActions
        // itself, for any file type, which is where the entries every
        // view shares actually live. Complementary, not a second
        // spelling — one checks the tab, the other checks the builder.
        QString assetMenu;
                               // TSV and quit (no window grab) — data probe
        bool exportMenu = false;   // open + screen-grab the Export menu, and
                                   // log its action texts (dev verification)
        int settleMs = 1200;
        bool stay = false;

        // True when anything OTHER than the dump now finishing still has work
        // to do — the other composable dump, a screenshot, a viewport capture,
        // a turntable or a per-part export. A dump that quits while one of
        // those is outstanding writes its own file, exits 0, and silently
        // produces nothing else. `except` names the caller's own field so it
        // does not see itself.
        bool moreOutputPending(QString DevShot::* except) const;
    };
    void scheduleDevShot(const DevShot& shot);

private:
    void buildMenus();
    void populateExportMenu();
    // Settings, and everything re-read on accept. Shared by the File menu and
    // the Ctrl+, hotkey.
    void openSettings(const QString& startTab);
    // Read app/Hotkeys.h and (re)install every bound shortcut as a window
    // QAction. Safe to call again — it replaces the previous set.
    void applyHotkeys();
    // §6's File ▸ Index and Help ▸ triage set.
    void showStatus(const QString& text);
    QString dumpMenuBar();
    void populateIndexMenu();
    void toggleLogConsole();
    void showShortcutSheet();
    void runHealthCheck();
    void exportLog();
    void copyDiagnosticInfo();
    // The asset-health audit. One implementation, two callers: --healthaudit
    // and Help ▸ Health check…. Returns rows written.
    int writeHealthAudit(const QString& tsvPath);
    void writeVariantCensus(const QString& tsvPath);
    // Ctrl+F: focus the search box of whichever tab is in front.
    void focusCurrentSearch();
    // Fire one Export-menu entry by role, through the menu the mouse uses.
    void triggerExportAction(const QString& role);
    void startRebuild();
    // Export ▸ Package mod folder…. Asks for a destination, writes the ZIP,
    // and reports the one number that decides whether the package is any use
    // to a loader: how many of its assets the game keeps inside a container.
    void exportModPackage();
    // Export ▸ Package as a SnakeBite mod…. Collects the metadata a mod
    // manager lists the mod under, then writes the .mgsv.
    void exportMgsvPackage();
    modpackage::MgsvMeta shotMgsvMeta(const QString& defaultName) const;
    void chooseGameFolder();
    void takeDevShot();

    QMenu* m_exportMenu = nullptr;
    QMenu* m_indexMenu = nullptr;
    // Created on first use and kept: a console that lost its scroll position
    // and its filter every time it was closed would be a console nobody keeps
    // open, which is the only way it is useful.
    fox::LogConsole* m_logConsole = nullptr;
    // The shortcut QActions this window owns. Held so a re-apply can delete
    // them: two QActions carrying the same sequence make Qt call the shortcut
    // ambiguous and fire NEITHER.
    QVector<QAction*> m_hotkeyActions;
    // The export report's "Show in folder" button and the folder it opens.
    // One of each: the button follows the LAST export, because that is the
    // one the message beside it is describing.
    QToolButton* m_exportShowBtn = nullptr;
    QString m_exportFolder;
    QTabWidget* m_tabs = nullptr;
    FilesTab* m_filesTab = nullptr;
    TexturesTab* m_texturesTab = nullptr;
    ModelsTab* m_modelsTab = nullptr;
    CustomizeTab* m_customizeTab = nullptr;
    BulkExtractorTab* m_bulkTab = nullptr;
    QLabel* m_statusLabel = nullptr;
    // The per-tab report (fox::StatusLine), and who made it — shown only
    // while that widget is inside the tab on screen.
    QLabel* m_tabStatusLabel = nullptr;
    QPointer<QWidget> m_tabStatusSource;
    QString m_tabStatusText;
    void syncTabStatus();

    DevShot m_shot;
    bool m_shotPending = false;
};
