// CustomizeTab.h — character composer: equip several FMDL parts (body, head,
// gear — soldier variants, Survive equipment, MGO outfits) into ONE viewport,
// and drive them all with a shared animation clip. Each part keeps its own
// skeleton; the rig (.frig) resolves bone drives per part by StrCode32 name,
// so a body and a head posed by the same clip stay in sync.
#pragma once
#include <QHash>
#include <QImage>
#include <QPair>
#include <QSet>
#include <QVector>
#include <QPointer>
#include <QWidget>

#include "anim/FrdvFile.h"
#include "fox/FcnpFile.h"
#include "model/GlbExporter.h"
#include "fox/FmdlFile.h"
#include "fox/FrigFile.h"
#include "fox/GaniAnim.h"
#include "fox/MtarFile.h"
#include "index/AvatarTextures.h"
#include "index/AvatarPresets.h"
#include "index/PartCatalog.h"
#include "preview/ModelLoader.h"
#include "preview/MaterialInspector.h"
#include "util/SceneTree.h"
#include "view/InfoPanel.h"
#include "view/NPanel.h"

class GLModelWidget;
class QCheckBox;
class AnimationsPanel;
class QComboBox;
class SearchableCombo;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QMenu;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTimer;
class QToolButton;
class FmdlListModel;

class CustomizeTab : public QWidget {
    Q_OBJECT
public:
    explicit CustomizeTab(QWidget* parent = nullptr);

    // Dev harness: add the first model matching each filter, then optionally
    // load an animation. Returns number of parts equipped.
    int equipParts(const QStringList& filters);
    bool selectAnim(const QString& mtarFilter, const QString& clipFilter,
                    float frame);
    // Dev harness: export the composed scene without a dialog.
    // Write the composed scene. `onlyPart` >= 0 restricts it to that part and
    // the models its variation attaches — which is what "one file per part"
    // is, and it is a filter on the existing walk rather than a second copy of
    // it, so the two exports can never disagree about what is in the scene.
    bool exportSceneTo(const QString& glbPath, int onlyPart = -1,
                       const QVector<glb::GlbAnimation>* anims = nullptr);
    // The composed scene, RIGGED, with one glTF animation per selected clip.
    // Each entry is (archive fileIdx, clip index), as the Animations panel
    // reports them. Returns the number of clips actually written.
    int exportSceneAnimatedTo(const QString& glbPath,
                              const QVector<QPair<int, int>>& clipSel,
                              QString* error = nullptr);
    // Resolve a clip SPEC against the archive this tab has open: empty = the
    // clip that is selected, "all" = every clip in it, otherwise a comma list
    // of indices and case-insensitive name substrings. Same grammar as the
    // Models tab's, deliberately.
    QVector<QPair<int, int>> clipsMatching(const QString& spec) const;
    // The batch export without the folder dialog — the harness cannot click.
    // Returns how many files were written.
    int exportPartsTo(const QString& dir);
    // Dev harness: switch to Weapon, set slots from "receiver[,slot=part…]"
    // and optionally a camo variation. Returns a human-readable result.
    QString buildWeaponFromSpec(const QString& spec, const QString& camo);
    // Same, for any builder category (1 = Weapon, 2 = Character,
    // 3 = Vehicle & buddy, 4 = every other character).
    // Open or close the material debug panel (the screenshot harness has no
    // way to press the button).
    void setDebugPanelVisible(bool on);
    // Open or close the submesh tree, and uncheck a row by label (harness).
    void setSubmeshTreeVisible(bool on);
    bool hideSubmesh(const QString& needle);
    // Re-read the Settings PBR switch into the viewport's own PBR state. Called when the
    // settings dialog is accepted, since the box is what the load path reads.
    void syncPbrFromSettings();
    // Type into the material panel's filter box (screenshot harness).
    void setMaterialFilter(const QString& text);
    // Drive the viewport's PBR box, exactly as clicking it would — including
    // the reload it triggers when the maps are not in hand.
    void setPbrShading(bool on);
    QString buildFromSpec(int categoryIndex, const QString& spec, const QString& camo);
    // --camodump <tsv>: every row refreshWeaponCamoList() would build, for
    // every weapon subject and variant in the index, with its section. A combo
    // popup is a top-level window and cannot be photographed, and the question
    // QUEUE 2 asks — does a weapon really ship both cam and clv, and is
    // anything else in MODEL VARIATION — is a census, not a look. Answers it
    // in one run on whatever install it is pointed at.
    QString camoDumpReport(const QString& tsvPath);
    // --restalignsweep <tsv>: equip EVERY item of every slot the current
    // subject has, one at a time, pose it on the loaded clip, and write how
    // far each one landed from the bone it hangs on. One double-click names
    // every accessory that is still in the wrong place, instead of one log
    // line for one item somebody had to think to check.
    QString restAlignSweepReport(const QString& tsvPath);
    // --camodefault: the four cases selectDefaultWeaponCamo() decides, run
    // against the real combo with a scripted list. The container ships no
    // chimera packs, so no weapon subject can be built here and the rule
    // cannot be reached through a scene — and an unverified selection rule is
    // how "clv by default" would ship as a claim rather than a behaviour.
    QString camoDefaultSelfTest();
    // The Unlocked switch, for the devshot harness. Not retroactive: it
    // governs the choices made after it, which is what the checkbox does too.
    void setGearUnlocked(bool on);

private:
    // Is anything in a slot a hat goes in? Decides which half of a folded
    // hairstyle pair is put in the scene.
    bool headgearWorn() const;
    // The covered form of the hairstyle whose model this is, or -1.
    int coveredHairFor(int fileIdx) const;

public:
    // Select a gear colour by its swatch stem ("cm_scol4_c17"). Returns false
    // when this install carries no such swatch. Exposed for the screenshot
    // harness, which has no way to click a combo.
    bool selectGearColor(const QString& swatchStem);
    // Apply a .fv2 variation to every fitted part BY NAME, bypassing the
    // Camo / Variation combo. The combo only offers the tables the catalogue
    // associates with the fitted stems, which is right for the screen and
    // wrong for a harness that needs to exercise a specific table — the models
    // a variation ATTACHES had no reachable test case at all until this
    // existed. Returns how many parts the named table actually resolved on.
    int applyFovaByName(const QString& fovaName);
    // Dev harness: apply the first preset whose label matches, and/or set the
    // compatibility switch. Returns a human-readable result.
    QString applyPresetFromSpec(const QString& presetFilter, bool compatOnly);
    // Same, for one of the user's OWN saved builds — the path that has to
    // survive the compatibility filter without losing parts.
    QString applyUserPresetFromSpec(const QString& name, bool compatOnly);

    // Save the built state under a given name and report the blob written.
    // For the harness: a preset is user data with a grammar of its own, and
    // until this existed there was no way to round-trip one.
    QString saveWeaponPresetAs(const QString& name);
    // Dev harness: drop open one builder combo so a screen grab shows the list.
    bool openBuilderPopup(const QString& which);
    // Dev harness: put the pointer on row `row` of the open popup so the hover
    // preview has something to fire on. Returns false when no popup is up.
    bool hoverBuilderPopupRow(int row);
    // Dev harness: type into the open popup to exercise the search filter.
    bool typeIntoBuilderPopup(const QString& text);
    // Dev harness: seat part `itemIdx` at `cnpName` of part `hostIdx`.
    bool attachPartTo(int itemIdx, int hostIdx, const QString& cnpName);

    // Contextual entries for the menu-bar Export menu (composed scene).
    // §6: the noun this tab's Export menu names its subject with — weapon,
    // character, vehicle or scene, from the category combo.
    QString exportSubjectNoun() const;
    void exportSceneToLast();

    // ── Exporting ONE slot, and exporting EVERY variation ────────────────
    // Dev harness: export the part fitted in `slot` on its own. Returns an
    // empty string on success, or the reason.
    QString exportSlotTo(const QString& slot, const QString& glbPath);
    // Dev harness: one file per row of the variation list, applying each in
    // turn. Returns how many were written; `error` is set when it refused.
    int exportVariationsTo(const QString& dir, QString* error = nullptr);
    // Dev harness: the slot menu as text. A menu is a top-level window and
    // never appears in a screenshot — the same reason --filemenu exists.
    QString slotMenuDump(const QString& slot);
    // Keep the ANIMATIONS panel in step with the animation bar.
    void syncAnimPanel();
    AnimationsPanel* m_animPanel = nullptr;
    // The per-part export entry, kept so its long tooltip can be attached
    // after the rest of the menu is built rather than duplicating the action.
    QAction* m_partsExportAction = nullptr;
    void populateExportMenu(QMenu* menu);

public slots:
    void onIndexReady(bool ready);

private:
    struct Part {
        int fileIdx = -1;
        QString path;
        fox::FmdlFile model;
        QVector<QImage> textures;
        QVector<QImage> normalMaps;
        // Full-PBR maps, EMPTY when the Customize viewport has PBR turned off.
        // Kept in step with `textures` by the same rule the normal maps are:
        // one entry per material or every later part's maps land on the wrong
        // material slot.
        QVector<GLPbrMaterial> pbr;
        frdv::FrdvFile frdv;   // sibling .frdv (help bones), when present
        bool hasFrdv = false;
        fox::FrigFile frig;    // this part's OWN rig (ModelDescription-bound)
        bool hasFrig = false;
        fox::FcnpFile fcnp;    // this part's connect points, when present
        bool hasFcnp = false;
        int attachPart = -1;   // ≥0: rigidly seated at another part's CNP
        QString attachCnp;
        QString fovaName;      // applied .fv2 variation ("cam", "clv", …)
        int boneBase = 0;      // this part's offset into the combined palette
        // Mesh groups this part must not draw — the bandanna welded onto an
        // avatar head, which the game only shows when it is equipped.
        QSet<int> hiddenGroups;
        // The same thing, but coming from the APPLIED .fv2 variation rather
        // than the avatar look. Kept SEPARATE and combined at draw time: a
        // variation change must not wipe the avatar's hiding, and picking a
        // different variation must not leave the previous one's hiding behind
        // — one shared set could not do both. Only applyFovaToPart() writes
        // these two, and it writes them on every call including the "none"
        // case, so deselecting a variation really does undo it.
        QSet<int> fovaHiddenGroups;
        // Models the applied variation BRINGS WITH IT — a hat, a bag, a hair
        // mesh. A .fv2 does not only swap textures and hide geometry: 478 of
        // the 1,895 shipped tables attach a model, 477 of them bound to the
        // wearer's own bones. They are held here rather than added to m_parts
        // so they live and die with the variation: picking another one, or
        // none, cannot leave last one's hat behind, and they cannot be
        // unequipped out from under the variation that asked for them.
        QVector<modelload::LoadedModel> fovaAttached;
        // The index each attached model came from, in step with the above, so
        // one already fitted as a part of its own can be skipped at draw time.
        QVector<int> fovaAttachedFiles;
        // Groups the variation turns explicitly ON. They win over the avatar
        // look's own hiding: the look hides a head's bandanna by a texture-path
        // heuristic, and a variation that names that group is stating a fact
        // the heuristic can only guess at.
        QSet<int> fovaShownGroups;
    };

    void addPart(int fileIdx, const QString& fovaName = QString());
    // Is this part's attachment `ai` a model the scene already draws — fitted
    // as a part of its own, or attached by another part? Compared by PATH: see
    // the note on the definition.
    bool attachmentAlreadyFitted(const Part& part, int ai) const;
    // Reload one part's textures with the named FOVA variation ("cam", "clv",
    // …) applied, or with none when the name is empty or that part has no
    // variation by that name. Resolved PER PART: material name hashes repeat
    // across weapons, so a variation authored for one model would happily —
    // and wrongly — substitute its textures into another.
    // `foundTable`, when given, reports whether this part actually HAS a table
    // by that name — distinct from whether the table changed anything. The
    // harness needs the difference: "applied to 2 parts" meant nothing while
    // it was counting the assignment rather than the resolution.
    // `forcedFile`, when >= 0, is the index of a .fv2 to read INSTEAD of
    // looking the name up in the active category's catalogue. Only the harness
    // sets it — see applyFovaByName for why a table can exist, attach a model,
    // and still be unreachable by name.
    void applyFovaToPart(int partIdx, const QString& fovaName,
                         bool* foundTable = nullptr, int forcedFile = -1);
    // Which builder slot a loaded part came from ("accessory", "hair", …), or
    // empty when it was not added through a slot row.
    // The narrow look pass for CLOTHING: its own skin materials follow the
    // chosen tone, and nothing else about the look touches it.
    void applySkinToneToPart(int partIdx);
    QString slotOfPart(int partIdx) const;
    void removePartAt(int row);   // remaps/detaches other parts' attachments
    void removeSelectedPart();
    // Which map set this viewport loads, from the Customize PBR setting.
    modelload::PbrMode pbrMode() const;
    // Re-run every fitted part's texture load at the current PBR mode without
    // disturbing the build. Used when the viewport's PBR box is ticked on a
    // character whose parts were loaded without the extra maps.
    void reloadPartMaps();
    // Rebuild the material panel from every part now in the scene. A no-op
    // when the panel is closed.
    void refreshInspector();
    // Push the roots rebuildScene collected into the tree widget.
    // Fill one N-panel by key, and every panel already open. See
    // ModelsTab::fillOpenPanels — restoreState() runs before the
    // panelOpenChanged connection exists.
    void fillPanel(const QString& key);
    void fillOpenPanels();
    void refreshSceneTree();
    void rebuildScene();
    void exportScene();   // save dialog + exportSceneTo
    // One .glb per equipped part, into a folder. The scene export puts them
    // all in one file; this is for the case where each piece has to arrive
    // separately.
    void exportPartsSeparately();
    // Directory dialog + exportVariationsTo.
    void exportVariations();

    // ── ONE builder for the slot menu ────────────────────────────────────
    // Two entry points ask the same question — "act on the part fitted here":
    // the equipped list's rows, and the slot combos on the builder panel.
    // They were about to become two menus with the same purpose and different
    // contents, which is the drift §4 exists to prevent. `slotLabel` is empty
    // for the equipped list, whose rows are not slots.
    //
    // An EMPTY slot still gets the header row and nothing else. A right-click
    // that opens nothing reads as a broken control rather than as an empty
    // slot, and a header is a statement rather than an action, so it does not
    // break §0's "never show an action it cannot perform".
    void addSlotMenuActions(QMenu* menu, int partIdx, const QString& slotLabel);
    // The export file stem for one part, INCLUDING its applied variation.
    // Without the variation in the name, exporting a weapon in three
    // camouflages writes three files nobody can tell apart.
    QString partExportStem(int partIdx) const;
    // The variation rows the export-every-variation pass would walk, as
    // (label, data). Empty when this build has no variation list.
    QVector<QPair<QString, QString>> variationRows() const;
    void populateAnimCombo();
    void locateFrig();
    void loadMtar(int fileIdx);
    void loadClip(int clipIdx);
    void setFrame(float f, bool fromSlider = false);
    // ── The animation GIF hook (§9) ─────────────────────────────────────
    // Same contract as ModelsTab's: installed on the viewport exactly while
    // there is a decoded clip AND something equipped to pose with it, cleared
    // otherwise, because the Camera page decides whether to offer "Save
    // animation GIF…" on the provider's presence alone.
    void syncAnimProvider();
    QVector<QImage> renderClipFrames(int frames);
    // Rigid attachment transform for part i at the current pose (identity
    // when unattached). `poses` may be empty (bind pose).
    animmath::Mat4 attachMatrixFor(
        int i, const QVector<QVector<animmath::Mat4>>& poses) const;
    void applyAttachTransforms(const QVector<QVector<animmath::Mat4>>& poses);

    // Rest-pose correction for a part whose root bone another part places
    // somewhere else — see the definition.
    //
    // `d` is a BIND-FRAME offset, and the bone it is measured against is as
    // much of the answer as the offset is. Applying `d` in the world frame
    // after skinning is right at bind and wrong the moment the anchor bone
    // rotates — by exactly d·R − d — which is what put the hair and the
    // goggles off the head as soon as a clip played while leaving the T-pose
    // perfect. So the anchor travels with the offset: setFrame folds
    // translate(d) into the part's palette IN FRONT of the bone's matrix, and
    // a part the clip drives nothing of is carried rigidly by anchorPart /
    // anchorBone instead of being left at bind.
    struct RestAlign {
        QVector3D d;
        int anchorPart = -1;   // index into m_parts, or -1
        int anchorBone = -1;   // bone index within that part's model
        // WHICH of restAlignmentFor's four exits produced this, because the
        // four are different claims about the data and a bare offset cannot
        // tell them apart:
        //   'b' another part carries this part's own root bone
        //   'c' the slot's connect point, on the point's own parent bone
        //   'a' the slot's anchor bone, no connect point found
        //   'f' FOXAB_FORCE_RESTALIGN
        //   '-' no correction (either not needed or not resolvable)
        char source = '-';
        bool isNull() const { return d.isNull(); }
    };
    RestAlign restAlignmentFor(int i) const;

    // ── FOXAB_DUMP_RESTALIGN (§13) ──────────────────────────────────────────
    // The rest-alignment pass, per part, on EVERY rebuildScene and on the
    // first frames after one. "Floating at load, wrong during a clip, right
    // again after playing one" and "the glasses are seated while the cap is
    // not, in the same frame" are two different failures and the only thing
    // that separates them is which part got which anchor and whether the clip
    // drives it. Both halves are printed because both are needed: the build
    // half is `d` and the anchor it was measured against, the frame half is
    // `drivenOf` and whether the part was carried rigidly or posed on its own.
    void dumpRestAlign();
    void dumpRestAlignFrame(int hostPart, float frame) const;

    // ── WHERE EVERY PART LANDED, LAST FRAME ─────────────────────────────
    // Filled by setFrame on every pose, whether or not any diagnostic is on:
    // it is a handful of multiplies per part and three different consumers
    // need it — the log dump, the --restalignsweep census, and anything later
    // that wants to answer "is this item seated" without a person looking at a
    // render. One computation, one spelling; a sweep that re-derived the
    // residual would drift from the pass exactly when it mattered.
    struct PartAlign {
        QString path, stem, slot, rootBone;
        int bones = 0;
        int driven = 0;
        // H host · B borrowed · C carried rigidly · O posed on its own
        // hierarchy · S seated on a connect point (the pass never sees it).
        char regime = '?';
        char source = '-';          // restAlignmentFor's exit: b/c/a/f/-
        int anchorPart = -1, anchorBone = -1;
        QVector3D d;
        // Metres between where the part's own root bone LANDED and where the
        // bone it hangs from actually is. Negative = not measurable (the host
        // itself, or a part with no comparable bone anywhere in the scene).
        float residual = -1.0f;

        // ── THE SAME QUESTION, ASKED OF THE GEOMETRY ────────────────────
        // `residual` above compares BONE positions, and that is the whole of
        // what it can see. A part whose root bone lands perfectly while its
        // vertices sit half a metre away scores 0.0000 on it — which is
        // exactly what a wardrobe sweep reported for a scene with two
        // accessories visibly off the head.
        //
        // So this measures the MESH. `meshOffset` is how far the part's
        // skinned centroid ended up from the bone it hangs on; `bindOffset`
        // is how far the author put it from that same bone in the part's own
        // file. A seated part reproduces what the author drew, so the two
        // agree, and `meshResidual` — the distance between the two offset
        // VECTORS — is the number that goes to zero when the part is where it
        // belongs. Comparing lengths alone would call a hat rotated to the
        // back of the head correctly placed.
        QVector3D meshOffset, bindOffset;
        float meshResidual = -1.0f;
        int meshVerts = 0;          // 0 = no skinned geometry to measure
    };
    void fillLastAlign(const QVector<int>& drivenOf,
                       const QVector<bool>& borrowedOf,
                       const QVector<RestAlign>& aligns,
                       const QVector<QVector<animmath::Mat4>>& poses,
                       const QVector<QVector<animmath::Mat4>>& unfolded,
                       int hostPart);
    static const char* alignRegimeName(char r);
    QVector<PartAlign> m_lastAlign;
    int m_restAlignBuild = 0;          // rebuildScene counter, 1-based
    // Per part path: what the previous build measured, so build N can be
    // compared against build N-1 in one line instead of by eye across a log.
    struct RestAlignPrev { QVector3D d; int anchorPart = -1; int anchorBone = -1;
                           char source = '-'; int build = 0; };
    QHash<QString, RestAlignPrev> m_restAlignPrev;
    // Armed by a build and by a clip load; counts down over setFrame so the
    // frame table appears for the frame the harness asked for as well as for
    // frame 0, and then stops rather than printing once per animation tick.
    int m_restAlignFrameDumps = 0;

    // Category switch: the tab is contextual, and the weapon builder is a
    // different shape of UI from the character composer (fixed slots driven by
    // the chimera catalogue vs a free-form part search).
    QComboBox* m_category = nullptr;
    QStackedWidget* m_stack = nullptr;
    // The builder panel is written once and driven by whichever category is
    // active — see fox::BuilderSource.
    fox::BuilderSource m_source;
    void setBuilderCategory(int categoryIndex);
    // Which builder category is live (1 = Weapon, 2 = Character,
    // 3 = Vehicle & buddy, 4 = every other character). Saved
    // presets and the compatibility switch are per category, so this is read
    // rather than m_category->currentIndex() (which is set before the source
    // has been swapped and would give the wrong answer mid-switch).
    int m_builderCategory = 0;
    // QSettings group for this category's saved builds. Weapon keeps its
    // original group so presets saved before this existed still load.
    QString presetGroup() const;
    void buildWeaponPanel(QWidget* parent);
    void refreshWeaponCatalogue();
    void rebuildSlotRows();
    // True when this page's parts carry MGO's Exclude/Must rules.
    bool subjectHasGearRules() const;
    void refreshGearRuleControl();
    // The footer's form layout (Camo, Gear Color, Games, Preset). Held so a
    // row down there can be hidden with its LABEL — labelForField only knows
    // the form the widget is actually in, and the slot rows are in another
    // one, which is how "Gear Color" came to sit on the page with no combo
    // beside it.
    QFormLayout* m_weaponFooterForm = nullptr;
    void onWeaponSlotChanged(int row);
    // Empty every slot, or fill each from what it offers.
    void setAllSlots(bool random);
    void onWeaponCamoChanged();
    void onWeaponColorChanged();
    // Drop the gear colour AND empty its combo. Both, always: clearing only
    // the held value left the drop-down showing a colour nothing was applying.
    void clearWeaponColor();
    // The chosen gear colour as its swatch's asset path, and the reverse.
    // Presets and outfits both persist the PATH rather than the hash: a hash
    // means nothing after a reindex. An empty string means "as shipped", and
    // setGearColorPath("") clears the colour rather than leaving the last one.
    QString gearColorPath() const;
    void setGearColorPath(const QString& swatchPath);
    // Fill the colour combo from the game's palette. Called once the index is
    // ready, since the swatches come out of the archives.
    void refreshWeaponColorList();
    void refreshWeaponCamoList();
    // A face preset sets more than the head model — apply the rest of it.
    // Apply what a face preset carries beyond the head itself: its hair, and
    // — when `reset` — every appearance row back to "from face preset", so a
    // new preset cannot inherit the previous one's explicit beard or brow.
    // The spec path passes false: it has already applied its own look fields.
    void applyFacePresetSideEffects(int headRow, bool reset);
    // The game's own "cannot be worn together" rule, from each MGO gear
    // item's Exclude list: after the part in `changedRow` is chosen, clear
    // any other slot whose fitted item conflicts with it — in either
    // direction, because the shipped lists are not symmetric. The newly
    // chosen item always wins.
    void applyGearExcludes(int changedRow);
    // Refresh m_look from whichever face preset is selected, then from any
    // appearance row the user has set explicitly.
    void updateLookFromHead();
    // Build and fill the appearance rows (skin tone, wrinkles, eyebrows, hair
    // colour, feature, eye shape) for a subject that has an avatar face.
    void buildLookRows();
    void fillLookRows();
    // The head model an explicit Eye Shape row asks for, or -1.
    int lookEyeShapeFile() const;
    // The hairstyle model's stem, or empty when no hairstyle is fitted. The
    // head needs it to lay that style's hairline over its own face map.
    QString selectedHairStem() const;
    // The head model stem this subject is currently wearing, e.g.
    // avf0_type1_def. Empty when there is no head row or no selection.
    QString lookHeadStem() const;
    // Which preset table this subject's head belongs to. Survive ships one per
    // gender and they are not interchangeable.
    fox::AvatarPresets::Sex lookSex() const;
    // Re-decode one part's base maps for the current avatar look. Every skin
    // tone, wrinkle set, hair colour, eyebrow and scar in this game is its own
    // texture, so a "look" is a set of texture substitutions, not shader state.
    void applyAvatarLookToPart(int partIdx);

    fox::AvatarLook m_look;        // the face currently being composed
    bool m_lookActive = false;     // …and whether it applies to this subject
    QString m_lookNote;            // what the last look substitution actually did
    // The label for one appearance variation. A name that carries the
    // camouflage index ("camo_c03") is looked up directly; one that does not
    // (a player FOVA slot, "v03") has its index read out of the texture the
    // table substitutes — see camoIndexFor(), which does that read once and
    // caches it by file index until the archives change.
    QString camoLabelFor(const fox::CatalogVariation& v);
    QHash<int, QString> m_camoIndexCache;
    void refreshWeaponList();          // the game's named weapons, then
                                       // every subject in the data
    void onWeaponPicked();             // base chosen: fill tiers/variants
    void onWeaponVersionChanged();
    void applyFamilyDefaults(const QString& familyId);
    void refreshWeaponPresets();
    // Fill the slot combos from the catalogue, optionally narrowed to what the
    // game says fits the fitted receiver/barrel. Selection is preserved.
    void refreshSlotItems();
    // Apply one of the game's own shipped builds (EquipCatalog).
    void applyGamePreset(int presetIndex);
    // The shared half: fit a build's parts with the base/tier already chosen.
    // Returns the stems this install does not carry.
    QStringList applyBuildParts(int presetIndex);
    void reportBuild(int presetIndex, const QStringList& missing);
    // The version combo becomes a STAR TIER list for one of the game's named
    // weapons; each row still carries the receiver's file index as its payload,
    // so every downstream reader is untouched.
    void fillTierList(int namedIdx);
    void fillVariantList(const QString& familyId);
    bool selectHostVariant(int fileIdx);
    int currentTierPreset() const;
    // The subject the contextual categories key their slots and parts on.
    QString currentSubjectId() const;
    QString m_currentSubjectId;
    // Model stem → file index, built once per index and cached.
    const QHash<QString, int>& stemIndex() const;
    mutable QHash<QString, int> m_stemIndex;
    mutable const void* m_stemIndexKey = nullptr;
    mutable int m_stemIndexCount = -1;
    QString pathForStem(const QString& stem) const;
    int fileIdxForStem(const QString& stem) const;
    // The model stem currently in a slot ("receiver" = the Version combo).
    QString currentStemFor(const QString& slot) const;
    void saveWeaponPreset();
    void loadWeaponPreset(const QString& name);
    // ── The one description of an authored scene (§15) ───────────────────
    // Slots, appearance rows, host, camo, gear colour and MGO dyes, as the
    // field grammar the preset system already spoke. Presets, outfits and the
    // undo stack are all this list — see captureSceneFields() for why there is
    // exactly one of them now. Sorted, so two equal scenes compare equal.
    // How a part is named in the per-part fields: its slot, or "#<path>" when
    // it has none (the subject's own body comes through __host, not a slot).
    QString partStateKey(int partIdx) const;
    QStringList captureSceneFields() const;
    void applySceneFields(const QStringList& fields);
    // The body. applySceneFields wraps it so one description is one undo step
    // however many times the scene settles on the way.
    void applySceneFieldsImpl(const QStringList& fields);

    // ── Undo, §15 ────────────────────────────────────────────────────────
    // Thirty deep, over the AUTHORED DESCRIPTION and never over m_parts: a
    // Part owns an FmdlFile, two QVector<QImage> texture sets, a FrigFile and
    // an FrdvFile, so thirty snapshots of those would be hundreds of megabytes
    // and a deep copy per click. A field list is a few hundred bytes.
    //
    // Nothing calls "push" at an authoring site. Instead every settled scene is
    // compared against the last settled one and a step is pushed only when the
    // description actually differs. That gets three things for free: a burst of
    // submesh clicks that ends in one state is ONE step; a rebuild the user did
    // not author (a PBR toggle, a camo refresh, a reindex) captures identically
    // and pushes nothing; and no authoring path can be forgotten, because none
    // of them is trusted to remember.
    static constexpr int kUndoDepth = 30;
    QVector<QStringList> m_undoStack;
    QVector<QStringList> m_redoStack;
    QStringList m_undoBaseline;      // the last settled description
    bool m_undoApplying = false;     // guards undo()/redo()'s own rebuild
    bool m_undoArmed = false;        // false until the first scene settles
    void noteSceneSettled();

public:
    // Bound to Ctrl+Z / Ctrl+Shift+Z by MainWindow, and only while this tab is
    // the one in front — an application-scoped Ctrl+Z that stepped the wardrobe
    // back while the user was on Textures would be a misfire, not a feature.
    void undoScene();
    void redoScene();
    QString undoSeqReport(const QString& seq);   // --undoseq

private:
    void deleteWeaponPreset();
    void rebuildWeapon();
    struct WeaponSlotRow {
        QString slot;
        SearchableCombo* combo = nullptr;
        int partIdx = -1;      // index into m_parts, or -1 when set to "None"
        // The SECOND model of a two-piece garment (see CatalogPart's
        // companionGearId), or -1. One row, one name, one icon — two models in
        // the scene and two more dye channels.
        int companionPartIdx = -1;
        QString companionGearId;
        bool unusable = false; // the game never fits anything here on this weapon
        bool seated = false;   // fitted AND placed on a connect point
        QString note;          // why it is not seated, when it is not
        // An APPEARANCE row rather than a part row. Its payload is an option
        // index into one of the avatar's texture sets, not a model file index,
        // and it adds nothing to the scene — it changes how the parts already
        // there are textured. Slot ids are prefixed "look:".
        bool isLook = false;
    };
    // Restyle the slot labels so the state of the build reads at a glance:
    // empty slots dimmed, seated ones normal, and anything fitted that could
    // not be placed marked and explained in its tooltip.
    void refreshSlotLabels();
    QVector<WeaponSlotRow> m_weaponRows;
    // ── MGO per-item gear colour ─────────────────────────────────────────
    // MGO3 dyes each worn item separately, two channels per item, from that
    // ITEM's own palette in GearConfig.lua. Each MGO gear slot gets two
    // colour rows, shown only while the slot holds an item with a palette.
    // Kept OUT of m_weaponRows: their payload is a colour id string, not a
    // model file index, and every loop over the slot rows assumes the
    // latter.
    struct GearColourRow {
        QString slot;                       // the gear slot it colours
        int channel = 0;                    // 0 = Primary, 1 = Secondary
        // TRUE for the two rows that dye the second model of a two-piece
        // garment. Each half of such a garment has its own palette in
        // GearConfig, so a slot holding one shows up to four dye rows; the
        // ones with no palette behind them stay hidden, which is why one of
        // these garments shows two rows and another three.
        bool companion = false;
        SearchableCombo* combo = nullptr;
    };
    // The key `m_mgoColours` uses for a slot's SECOND model. A distinct key
    // rather than two more fields, so every existing reader — the preset
    // save/restore, the spec parser, the clear-on-change — keeps working
    // unchanged on both halves.
    static QString companionColourKey(const QString& slot)
    {
        return slot + QStringLiteral("\u001Fchest");
    }
    QVector<GearColourRow> m_gearColourRows;
    // slot id → explicitly chosen (primary, secondary) colour ids. Absent or
    // empty = the item's own default, which is what its shipped textures
    // already show — nothing is applied for it.
    QHash<QString, QPair<QString, QString>> m_mgoColours;
    // Fill/show/hide the colour rows from what the gear slots currently
    // hold; resets a slot's stored choice when its item changed.
    void fillGearColourRows();
    void onGearColourChanged(int rowIdx);
    // The colour choices that apply to the part built from this model file,
    // resolved through the gear slot whose combo selected it.
    QPair<QString, QString> mgoColoursForPart(int fileIdx) const;
    // /Assets/mgo/fova/chara/*/<colourId>.fv2 in the index, or -1. Cached per
    // index generation — applyFovaToPart asks on every texture pass.
    int mgoColourFovaIndex(const QString& colourId);
    QHash<QString, int> m_mgoColourFv2;
    const void* m_mgoColourFv2Key = nullptr;
    int m_mgoColourFv2Count = -1;
    // slot → the file index a restore path is about to select, so the
    // compatibility filter cannot drop it before it gets there.
    QHash<QString, int> m_pendingKeep;
    // slot → the model this subject wears when nothing is chosen. Row 0 of a
    // slot with a default IS that part, so an "empty" character is dressed.
    QHash<QString, int> m_slotDefaults;
    QWidget* m_weaponPanel = nullptr;
    QFormLayout* m_weaponRowsForm = nullptr;
    SearchableCombo* m_weaponPick = nullptr;      // "Base": the game's named
                                       // weapons by class, then every subject
    SearchableCombo* m_weaponVersion = nullptr;   // "Tier": that weapon's star
                                       // grades, or a subject's bare variants
    SearchableCombo* m_weaponPreset = nullptr;   // the game's builds + your own
    QPushButton* m_weaponSavePreset = nullptr;
    QPushButton* m_weaponDeletePreset = nullptr;
    // "Color": the customize screen's colour menu — the game's own swatch art,
    // split into the two categories it uses.
    SearchableCombo* m_weaponCamo = nullptr;
    // The game's own colour palette, applied to every colour-customizable
    // material on every fitted part. Separate from m_weaponCamo because the
    // two are different mechanisms: a camouflage is one model's authored .fv2
    // table, a colour is a single texture rebind that works on anything whose
    // shader multiplies a layer.
    SearchableCombo* m_weaponColor = nullptr;
    // PathCode64 of the chosen swatch, 0 = leave every model as it ships.
    // Held rather than read back from the combo so applyFovaToPart(), which
    // runs from several paths, has one place to ask.
    quint64 m_gearColor = 0;
    // The UI swatch texture for one variation, or empty. Cheap for a weapon or
    // vehicle (the index is in the name); a character's "vNN" slot costs one
    // archive read the first time, cached thereafter. Never called from paint.
    QString swatchPathFor(const fox::CatalogVariation& v);
    // The camouflage index ("c05") a variation carries or substitutes.
    // ── ONE SPELLING OF THE SECTION SPLIT ───────────────────────────────
    // The customize combo's four captions and the --camodump census both have
    // to classify a variation, and the split is the GAME's own naming rather
    // than a guess. It lived inline in refreshWeaponCamoList; a census that
    // re-implemented it would be the second spelling this project keeps
    // deleting, so it is a function and both callers ask it.
    enum class CamoSection { CamoPattern, BaseColor, SkinTone, ModelVariation };
    CamoSection camoSectionFor(const fox::CatalogVariation& v);
    static const char* camoSectionCaption(CamoSection s);
    // Which row a freshly built camo list opens on: what was selected
    // before, then "clv" for a WEAPON, then the model's own textures.
    void selectDefaultWeaponCamo(const QString& previous);
    QString camoIndexFor(const fox::CatalogVariation& v);
    QCheckBox* m_weaponCompat = nullptr;   // narrow slots to what the game allows
    // Ticked: the game's Exclude/Must rules stop MOVING slots for you. They
    // still describe the data and are still logged; nothing else changes —
    // not the lists, not the gender, not which slot an item goes in.
    QCheckBox* m_gearUnlocked = nullptr;
    // Up while applyGearExcludes is applying the cascade from its own Must
    // pass, so a required item's rules run once and cannot re-enter.
    bool m_applyingGearRules = false;
    // Defined in the .cpp: QCheckBox is only forward-declared here.
    bool gearRulesLocked() const;
    // Which games the builder draws from — one install can hold four.
    QWidget* m_gameRow = nullptr;
    QLabel* m_weaponInfo = nullptr;
    QLabel* m_weaponPickLabel = nullptr;
    QLabel* m_weaponVersionLabel = nullptr;
    bool m_weaponRebuilding = false;

    // Browser side.
    QLineEdit* m_search = nullptr;
    QListView* m_list = nullptr;
    FmdlListModel* m_listModel = nullptr;
    QPushButton* m_addBtn = nullptr;

    // Equipped parts.
    QListWidget* m_equipped = nullptr;
    QPushButton* m_removeBtn = nullptr;
    QVector<Part> m_parts;
    // Draw only the HEAD of this scene part, and hide this scene part outright.
    // Both are held as a PART INDEX rather than a bool because "part 0" is not
    // reliably the base body — a build whose base model failed to load makes
    // part 0 the first slot item, and head-filtering a torso deletes it — and
    // because rebuildScene() runs from several places (a camo change, for one),
    // so a decision applied only after rebuildWeapon()'s own call is silently
    // lost by the others. -1 means "no part".
    int m_headOnlyPart = -1;
    int m_hideBasePart = -1;
    // Per-contributor pose palettes for the CURRENT frame, keyed by
    // (part index, attachment index) with -1 for the part itself. Written by
    // setFrame() as it walks the scene and read by exportSceneTo().
    //
    // Keyed rather than positional on purpose. setFrame concatenates one flat
    // palette in a strict layout that rebuildScene has to match exactly, and
    // the export needing the same matrices was previously answered by a SECOND
    // copy of the posing rules — which knew about parts and not about the
    // models a variation attaches, so a hat exported in bind pose under an
    // animated head. One producer, looked up by name, cannot drift.
    QHash<QPair<int, int>, QVector<animmath::Mat4>> m_framePalettes;
    // Scene submesh id → which contributor owns it and its index WITHIN that
    // contributor's model. Same one-producer rule as the palettes above:
    // rebuildScene lays the scene out and records the mapping as it goes, and
    // the export reads it rather than re-deriving the layout — re-deriving is
    // what put a hidden submesh into a .glb that the viewport was not drawing.
    //   key   scene mesh id (what SceneTree and GLModelWidget speak)
    //   value ((part index, attachment index or -1), local mesh index)
    QHash<int, QPair<QPair<int, int>, int>> m_meshOwner;

    // Saved outfits (QSettings "customize/…", identified by part PATHS).
    QComboBox* m_outfits = nullptr;
    QPushButton* m_saveOutfitBtn = nullptr;
    QPushButton* m_deleteOutfitBtn = nullptr;
    void refreshOutfits();
    void saveOutfit();
    void loadOutfit(const QString& name);
    void deleteOutfit();

    // Viewport + overlays.
    void applyViewportFullscreen(bool on);
    // Fill the INFO panel from the composed scene. A no-op when the panel is
    // closed — an open panel is what pays for its contents.
    void refreshInfoPanel();

public:
    // Dev harness: open exactly these panels and no others. Same contract as
    // ModelsTab::setPanelsForShot — one implementation would need a shared
    // base this tab does not have, so the two are deliberately identical in
    // shape and each is four lines.
    QString setPanelsForShot(const QString& keys);

private:

    GLModelWidget* m_view = nullptr;
    QWidget* m_tip = nullptr;
    QVector<QPointer<QWidget>> m_fsHidden;
    // Wireframe, Skeleton, Normal maps and PBR were four glyph toggles here.
    // They are on the viewport now — the shading balls and the Graphics
    // popover — so this tab keeps only the RELOAD they can ask for, guarded by
    // the flag below: reloadPartMaps() re-asserts the shading state as it
    // finishes, which emits displayChanged again, and without the guard that
    // second emission asked for another reload.
    bool m_reloadingForPbr = false;
    // The N-panel column and the panels in it (template §6). What used to be
    // two panes with their own toolbar buttons.
    fox::NPanel* m_npanel = nullptr;
    fox::InfoPanel* m_infoPanel = nullptr;
    MaterialInspector* m_inspector = nullptr;
    // The submesh tree. Scene-wide ids, re-based per part in rebuildScene the
    // same way material slots and bone bases are.
    SceneTree* m_sceneTree = nullptr;
    // Built by rebuildScene alongside the uploads, so the tree describes
    // exactly what was drawn — including which attachments were skipped.
    QVector<SceneTree::Node> m_treeRoots;
    // NOT in any layout and never shown — see ModelsTab.h and
    // app/StatusLine.h. setStatus() writes here and to the status bar.
    QLabel* m_info = nullptr;
    // The tab's status line — the window's status bar. See app/StatusLine.h.
    void setStatus(const QString& text);

    // Animation.
    // Grouped, searchable organizers — see util/AnimCombo.h, which fills both
    // of these and the Models tab's pair from one place. Captions are rows, so
    // a combo INDEX is not a clip index: every read goes through
    // currentPayload() / selectPayload().
    SearchableCombo* m_mtarCombo = nullptr;
    SearchableCombo* m_clipCombo = nullptr;
    QToolButton* m_playBtn = nullptr;
    QSlider* m_frameSlider = nullptr;
    QLabel* m_frameLabel = nullptr;
    QTimer* m_animTimer = nullptr;
    fox::MtarFile m_mtar;
    fox::GaniAnim m_anim;
    fox::FrigFile m_frig;
    bool m_hasMtar = false;
    bool m_hasAnim = false;
    // Animated-export scratch: which scene part came from which of m_parts,
    // and which (clip, sample) the scene is currently posed at, so a whole
    // scene is posed once per frame rather than once per part per frame.
    // (part index, attachment index) — -1 for the part itself. A variation's
    // attached model is a scene part with a palette of its own, so the owner
    // has to name both halves or an animated export cannot answer for it.
    QVector<QPair<int, int>> m_scenePartOwner;
    // How many clips the writer actually put in the file, as opposed to how
    // many were asked for. Reported to the user instead of the request.
    int m_clipsWritten = 0;
    int m_exportClip = -1;
    int m_exportSample = -1;
    bool m_hasFrig = false;
    bool m_frigSearched = false;
    bool m_recenterPending = false;
    // Set when a clip loads; cleared by the first setFrame after it. Makes the
    // per-part "is anything driving this" report fire once per clip instead of
    // once per frame.
    bool m_animDiag = false;
    float m_frame = 0.0f;
};
