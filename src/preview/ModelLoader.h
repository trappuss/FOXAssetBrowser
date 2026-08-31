// ModelLoader.h — shared FMDL → viewport pipeline used by the Files preview,
// the Models tab and the Customize composer: parse the model, fetch + decode
// its base-color and normal-map textures, build interleaved GPU uploads and the skeleton
// overlay. One implementation so every viewer agrees.
#pragma once
#include <QImage>
#include <QString>
#include <QVector>

#include <QHash>
#include <QSet>
#include <QPair>

#include "fox/FmdlFile.h"
#include <QSet>

#include "fox/FovaFile.h"
#include "gl/GLModelWidget.h"
#include "index/AvatarTextures.h"

namespace modelload {

// A FOVA variation resolved against one model: (material index, texture role
// StrCode32) -> replacement texture PathCode64. Built by fovaOverrides() and
// consulted by the texture loaders, so a camo/paint variation costs nothing
// when absent and needs no second code path when present.
using FovaOverrides = QHash<QPair<int, quint32>, quint64>;

// Resolve a parsed .fv2 against THIS model: FOVA addresses materials by name
// hash, which has to be mapped onto this model's material indices.
// `matched` (optional) reports how many substitutions actually landed — 0 means
// the variation was authored for a different model.
FovaOverrides fovaOverrides(const fox::FmdlFile& model, const fox::FovaFile& fova,
                            int* matched = nullptr);

// The OTHER half of a variation: which of this model's mesh groups it turns
// off, and which it turns explicitly back on.
//
// A .fv2 is not only a texture swap. It carries a list of mesh groups to hide
// and a list to show, by StrCode32 of the group name — that is how a costume
// drops the bandanna welded onto a head, and how Survive's headgear stows its
// gas mask ("MESH_mask"). Both lists come back as mesh-group INDICES into
// `model.meshGroups()`, ready to hand to a viewer's per-group visibility.
//
// The two are returned SEPARATELY rather than pre-merged. A caller that hides
// groups for its own reasons has to be able to see a variation say "no, show
// that one" and act on it — merging here would make an explicit show
// indistinguishable from silence. A group named by both lists comes back only
// in `show`, matching the order the static block stores them in.
//
// Groups the table names but this model does not have are ignored: a variation
// authored for a sibling model says nothing about this one.
void fovaGroupVisibility(const fox::FmdlFile& model, const fox::FovaFile& fova,
                         QSet<int>* hide, QSet<int>* show);

// How much of a material to load.
//
// Fox binds up to seven maps per material. Decoding all of them costs real
// time — the BC decode is on the CPU — and for browsing a list of models the
// extra maps buy nothing. Basic is what this loader always did: the base
// colour map and the normal map, nothing else. Full adds the SRM, the TRM and
// the layer pair, which is what the PBR shader needs to light a surface the
// way the game does.
enum class PbrMode { Basic, Full };

struct LoadedModel {
    bool ok = false;
    QString error;
    fox::FmdlFile model;
    QVector<QImage> textures;        // per material instance (base color)
    QVector<QImage> normalMaps;      // per material instance (RGB normal map)
    // Per material instance, and EMPTY under PbrMode::Basic. Never rely on
    // size() here to learn the material count — use model.materials().
    QVector<GLPbrMaterial> pbr;
    QVector<GLMeshUpload> uploads;
    GLSkeletonUpload skeleton;
    int texturesFound = 0;
    int normalMapsFound = 0;
    int pbrMapsFound = 0;            // individual maps, not materials
};

// The models a variation BRINGS WITH IT, loaded and ready to draw.
//
// A .fv2 can attach an extra model to the thing it varies — a hat, a bag, a
// hair mesh. Measured over the 1,895 shipped tables: 478 attach something, and
// every attached file is an .fmdl. 477 of the 490 attachments are the BONE
// form, where the model is skinned to the wearer's own skeleton and needs no
// transform at all; the remaining 13 name a connection point (CNP_HEAD and
// CNP_EYE, plus three hashes that resolve to no name) and are skipped here,
// because seating them wants the wearer's .fcnp and that is a different job.
//
// `skipped` (optional) reports how many the table declared that this function
// did not return — a model this install does not carry, or a connect-point
// attachment. A caller that says "attaches 2 models" when the table asked for
// 4 is lying quietly, which is the failure mode this exists to prevent.
// `fileIdx` (optional) receives the index of each returned model, in step, so
// a caller can tell that an attachment is a model it has ALREADY equipped. It
// usually is: a character variation's job is to compose the character, and
// avm0_type0_v00 both hides the body's baked head and attaches the very head
// model the browser fits from its own catalogue. Drawing both is a doubled
// head, so the check is not optional in practice.
// `mode` and `gearColor` are passed through to each attached model so it is
// loaded exactly like the part that attached it. They are not optional
// niceties: an attachment loaded basic into a full-PBR scene renders with the
// shader's default roughness and no occlusion next to geometry that has both,
// and one loaded without the gear colour stays white on a character that has
// just been painted.
QVector<LoadedModel> fovaAttachedModels(const fox::FovaFile& fova,
                                        int* skipped = nullptr,
                                        QVector<int>* fileIdx = nullptr,
                                        PbrMode mode = PbrMode::Basic,
                                        quint64 gearColor = 0);


// Load an indexed .fmdl (by ArchiveIndex file index) ready for display.
LoadedModel load(int fileIdx, PbrMode mode = PbrMode::Basic);

// The pieces, for callers that already have a parsed model:
// Per-material decal: after a material's base map is chosen, this texture is
// composited over it using its own alpha. That is how the avatar's scars,
// tattoos and face paint reach the face — the game layers them, there is no
// shipped face texture with the scar already in it.
// A material can need MORE than one, and the face needs exactly that: the
// avatar's beard is painted onto the skin as well as onto its own mesh, so a
// bearded face with a scar carries two decals at once. They are composited in
// list order.
using TextureOverlays = QHash<int, QVector<quint64>>;  // material → PathCode64s

// One per material and never a list: an eye has exactly one iris, and unlike a
// decal it is masked to a measured disc rather than blended by its own alpha.
using TextureIris = QHash<int, quint64>;

// Per-material FORCED base map: used where a material has no base-colour
// reference at all to substitute into. The avatar's eyebrow material is exactly
// that — it carries a normal and a specular map and nothing else, because the
// game binds the brow texture at runtime — so without this it can only ever
// render white, which is what it did.
using TextureForce = QHash<int, quint64>;

QVector<QImage> loadBaseTextures(const fox::FmdlFile& model, bool gz, int* found,
                                 const FovaOverrides* fova = nullptr,
                                 bool lowRes = false,
                                 const TextureOverlays* overlays = nullptr,
                                 const TextureForce* force = nullptr,
                                 // Iris maps, composited into the eyeball's own
                                 // iris disc rather than laid over the whole
                                 // map — see compositeIris().
                                 const TextureIris* iris = nullptr);

// Build the texture substitutions that turn a freshly loaded avatar head or
// hair model into ONE look: the chosen skin tone and wrinkle set on the face,
// the chosen eyebrow shape and colour, the chosen hair colour, and the chosen
// facial feature laid over the face. Materials are classified by the asset path
// their own texture reference resolves to — no material index is assumed.
// `classified` (optional) reports how many materials were recognised.
// `base` is the part's OWN variation table (its .fv2), which is where the real
// face, eyebrow and hair maps come from at all: an avatar head model ships with
// 128x128 placeholder textures bound and names none of its real ones, so
// without the variation there is nothing to rewrite and nothing to show.
// `hideGroups` receives the mesh groups that must not be drawn — on an avatar
// head that is the bandanna the model ships welded on.
void avatarOverrides(const fox::FmdlFile& model, const QString& modelStem,
                     const fox::AvatarLook& look, const FovaOverrides* base,
                     FovaOverrides* out, TextureOverlays* overlays,
                     TextureForce* force, QSet<int>* hideGroups,
                     int* classified = nullptr,
                     // Substitutions for the NORMAL map slot, to be handed to
                     // loadNormalMaps(). The wrinkles live in the normal map,
                     // so a face whose colour is swapped and whose normal is
                     // not comes out smooth no matter which set is chosen.
                     FovaOverrides* outNrm = nullptr,
                     // Per-eye iris substitutions. The eye material's Base_Tex
                     // is the EYEBALL (sclera plus a grey iris disc); the
                     // coloured iris is a second map the game's shader masks
                     // into that disc, so it cannot be a plain base swap and
                     // gets its own composite. Key is the material index.
                     TextureIris* iris = nullptr,
                     // Substitutions for the SPECULAR (SRM) slot, to be handed
                     // to loadPbrMaps(). Every one of these materials binds a
                     // flat placeholder there — or nothing at all — because the
                     // game binds the real map when it picks the look, and
                     // without this the shader falls back to occlusion 1 and a
                     // flat roughness 0.55 on the face, the hair, the brows and
                     // the beard. See AvatarTextures::faceSpecularPathFor.
                     FovaOverrides* outSrm = nullptr);

// Everything a THUMBNAIL needs and nothing else: the model, its base colour
// maps at mip resolution, and the uploads. No normal maps (the icon shader has
// no use for them and they cost as much as the base maps), no skeleton, no
// full-resolution decode. Measured on the Phantom Pain character models, the
// full load takes 3.6–8.8 SECONDS each; this is a small fraction of it.
LoadedModel loadForThumbnail(int fileIdx);
// Tangent-space normal maps, unswizzled from Fox's DXT5nm encoding (x in
// ALPHA, y in GREEN) into ordinary RGB888 normal maps. The unswizzle happens
// here, before any resize, because Qt's smooth scaler premultiplies by alpha —
// which in a DXT5nm map is the x channel, so scaling the raw form corrupts y.
QVector<QImage> loadNormalMaps(const fox::FmdlFile& model, bool gz, int* found,
                               const FovaOverrides* fova = nullptr);

// The rest of the PBR set: the SRM, the TRM and the layer pair, one entry per
// material instance, plus the shader-derived flags that say how to combine
// them. `found` (optional) counts the individual MAPS that decoded, not the
// materials — a material with only an SRM still contributes.
//
// Every map goes through the same FOVA override table as the base and normal
// maps, which is the whole point: a weapon camouflage IS a Layer_Tex
// substitution, so colouring a model is loading it with the right overrides
// rather than a separate code path.
//
// Maps are capped at 512 px. They are all low-frequency (an SRM is a bell of
// roughness values, a layer swatch is usually one flat colour), and four extra
// 1024x1024 RGBA maps per material would cost more video memory on one
// character than every other texture in the scene put together.
QVector<GLPbrMaterial> loadPbrMaps(const fox::FmdlFile& model, bool gz,
                                   int* found = nullptr,
                                   const FovaOverrides* fova = nullptr);

// Paint every colour-customizable material on this model with one of the
// game's own colour swatches.
//
// A customizable garment ships WHITE: its Layer_Tex_SRGB points at
// cm_flat_white (315 of the 412 layer references in the shipped models do) and
// the game rebinds that one slot to a flat colour, which its shader multiplies
// through the model's own layer mask. This produces exactly that rebinding, as
// an override table, so colouring a model runs through the same path a FOVA
// camouflage does instead of needing one of its own.
//
// Only materials whose SHADER asks for a layer are touched — a "LayerMul" or
// "LayerBl" name. A material that happens to carry a layer texture its shader
// never reads is left alone, because rebinding it would change a slot nothing
// samples on the models where it does nothing and tint the wrong surfaces on
// the models where it does.
//
// `painted` (optional) reports how many materials were rewritten. Zero means
// this model has nothing customizable on it, which is a fact about the model,
// not a failure.
FovaOverrides layerColorOverrides(const fox::FmdlFile& model,
                                  quint64 swatchPathHash, int* painted = nullptr);

// How many of a model's materials the game would let you colour. The honest
// answer to "is this piece of gear customizable", and the same test
// layerColorOverrides() applies.
int colourableMaterialCount(const fox::FmdlFile& model);

// Which of a model's materials are BARE SKIN — the ones that follow the
// avatar's chosen tone rather than their own shipped map.
//
// The test is the one avatarOverrides() applies, exported so the two cannot
// drift apart: a skin material binds a Translucent map (subsurface) and NO
// Layer pair (the colourable one), and does not name the beard or eyebrow
// folders — which the Fox skin shader would otherwise let through, since the
// beard is drawn with it. Measured across MGO's 195 character models: it picks
// exactly the material the artists named "color_skin" on the nine garments per
// gender that leave arms or a neck bare, and no cloth material anywhere.
QVector<int> skinMaterials(const fox::FmdlFile& model);

// The bare-skin substitution ON ITS OWN, for a part that is CLOTHING.
//
// A garment must not go through avatarOverrides(). That function is written
// for a head and several of its passes have no head-only guard — the eyebrow
// fallbacks in particular will claim any material binding exactly base, normal
// and specular, and then either paint the eyebrow atlas onto it or hide its
// mesh group. Measured: 31 materials on MGO's garments and hats match that
// shape, one of them on the fatigues. So the one thing a garment does need —
// its exposed skin following the chosen tone — is done here instead, and
// nothing else about the look touches it.
//
// `bodyPathNoExt` is AvatarTextures::bodyPath(); an empty one is a no-op.
void skinToneOverrides(const fox::FmdlFile& model, const QString& bodyPathNoExt,
                       FovaOverrides* out, TextureForce* force);
// The DXT5nm -> RGB888 conversion on its own (exposed for tests/tools).
QImage unswizzleDxt5nm(const QImage& src);
QVector<GLMeshUpload> buildUploads(const fox::FmdlFile& model);
// Drop every triangle that is NOT skinned to the subtree under `rootBone`
// (a bone StrCode32), in place. Used to keep only the HEAD of a base body that
// is being worn under a full set of clothing: drawing the whole mannequin
// underneath a dressed character makes every surface z-fight with the garment
// on top of it. Vertices are left alone — only the index lists shrink — so
// nothing downstream has to be renumbered. A no-op when the bone is absent.
void keepBoneSubtree(QVector<GLMeshUpload>& uploads, const fox::FmdlFile& model,
                     quint32 rootBone);
GLSkeletonUpload buildSkeleton(const fox::FmdlFile& model);

}  // namespace modelload
