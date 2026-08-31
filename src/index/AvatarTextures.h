// AvatarTextures.h — the avatar's face, eyebrow, hair and facial-feature maps.
//
// None of these is a shader effect. Every skin tone, every wrinkle set, every
// hair colour and every scar is its OWN texture, and the game picks one by
// name. The grammar, measured against the shipped archives rather than assumed:
//
//   face   .../face/avf0_type0_v<WW>_c<SS>_bsm        WW = wrinkles, SS = skin
//   brows  .../ebrw/avm_ebrw0_<shape>_<col>_bsm_alp   shape a0…k0, col bk0…wh0
//   hair   .../hair/avf_hair<N>_<style>_<col>_bsm_alp
//   scar   .../acce/avm_gash0_v<II>_c<SS>_bsm_alp     per skin tone
//   tattoo .../acce/avm_tato0_v<II>_c<SS>_bsm_alp
//   paint  .../acce/avm_fpnt0_v<II>_c<SS>_bsm_alp
//
// In a Metal Gear Survive install these live under /Assets/tpp/chara/avm/, not
// /Assets/ssd/ — Survive carries MGSV's avatar texture library and its own
// /Assets/ssd/chara/avm/Pictures holds only a couple of detail maps. Three
// roots are probed — ssd, then tpp, then mgo — and whichever answers first is
// used, so this does not care which game's folders are configured.
//
// The sets are ENUMERATED by probing the index for each name in the grammar,
// never hard-coded: an install with a different number of skin tones reports a
// different number of skin tones.
//
// The face-preset table (AvatarPresets) chooses one of each per preset; the
// scar is composited over the face map with its own alpha, which is how a
// single face texture can carry any of the features.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace fox {

// One complete face: which of each set to use. Filled from a preset, then
// overridable one field at a time.
struct AvatarLook {
    int wrinkle = 0;         // face v index
    int skin = 0;            // face c index
    int browShape = -1;      // index into browShapes(); -1 = leave alone
    int browColour = -1;     // index into hairColours()
    int hairColour = -1;     // index into hairColours()
    // The hairstyle MODEL's stem ("avf_hair_a0_v0_cov"), so the head can lay
    // that style's hairline over its own face map. Empty = no hairstyle, and
    // no hairline with it.
    QString hairStem;
    int beard = -1;          // index into beardShapes(); -1 = none resolved
    // Did the LOOK ask for a beard at all? Distinct from `beard >= 0`, which
    // also goes negative when the preset names a beard this install does not
    // ship — and the two want opposite treatment: no beard asked for means
    // hide the mesh, a beard asked for and not found means leave it alone.
    bool beardWanted = false;
    int decoType = -1;       // 0 = scar (gash), 1 = tattoo, 2 = face paint
    int decoId = -1;         // the v index within that family
    // Iris colour, per eye, as an index into irisColours(); -1 = leave the
    // model's own. The game really does hold these separately — the men's
    // preset 17 is one copper eye and one white one — so they are two fields
    // and not one.
    int eyeColourR = -1;
    int eyeColourL = -1;
    // The shade flag that travels with each: 0 is the bright print of that
    // colour, 1 the dark one. Indexes irisShades().
    int eyeShadeR = 0;
    int eyeShadeL = 0;
    // This part is BARE SKIN (a default arm/leg/torso), so its map follows the
    // face's tone. Never set for a clothed part.
    bool bareSkin = false;
    // Which game the BARE PART belongs to. The shared body-skin maps are
    // avatar art — avm0_body0_def in TPP, avf0_body0_def in MGO — painted for
    // the avatar's own mesh and UV layout. Survive's survivors are a different
    // cast on different meshes, and its default bare limbs (lgm0, lgm1, lgf0,
    // arm0, arf0, bdf0 — the only ones that ship no base map of their own)
    // were being handed the TPP avatar's map, which is what "the male leg
    // texture is broken / the UVs look flipped" actually is. Survive ships no
    // body skin at all in these archives: /Assets/ssd/chara/avm/Pictures holds
    // face and hair and nothing else.
    bool avatarGame = true;
    // Is a bandanna equipped? Every avatar head ships one welded on in a group
    // named MESH_bdn_IV, and the game only draws it when the matching ITEM is
    // worn — hat21_main0_def (hat_m21 / hat_f21 in MGO's gear table) is that
    // item. So the head's own bandanna is not scenery to be deleted: it is the
    // bandanna, and equipping the item is what turns it on.
    bool bandana = false;
    // Is `skin` a tone somebody CHOSE, or just the first one in the install?
    //
    // This exists because m_lookActive gates far more than the face: it also
    // gates the shared body-skin substitution, which paints the AVATAR's body
    // map onto a character's bare limbs. Letting the look run on pages that
    // never had it — which is what made the face textures and the bandanna
    // work — also switched that substitution on for them, and the avatar body
    // map lives under /Assets/tpp/. The result was TPP's skin arriving on
    // bodies that had been using their own.
    //
    // So the body substitution is gated on this and everything else is not. A
    // look with no chosen tone still textures a face, still hides a bandanna,
    // still lets the appearance rows drive; it just does not repaint anybody's
    // body on the strength of a default nobody asked for.
    //
    // TRUE FOR BOTH KINDS OF CHOICE, which the first version of this got
    // wrong. A face preset names a tone, so a preset sets it — but so does the
    // user picking one in the Skin Colour row, and that row is present on
    // pages whose head is NOT in the numbered grid (buildMgo appends the
    // un-numbered heads after the grid, so the row exists while the head
    // carries no preset index). Gating on "a preset seeded this look" therefore
    // ignored a deliberate choice and left the face retoned over an untouched
    // body.
    bool skinChosen = false;
    // The SUBJECT's gender, for the art that ships once per gender under a
    // shared name — the bare-skin body maps. Derived from the character being
    // built, not from the part's own stem: a survivor can wear the other
    // gender's gear, and the skin under it is still his or hers.
    bool men = false;
};

class AvatarTextures {
public:
    static const AvatarTextures& instance();

    bool ok() const { return !m_wrinkles.isEmpty() || !m_skins.isEmpty(); }
    QString note() const { return m_note; }

    const QVector<int>& wrinkles() const { return m_wrinkles; }
    const QVector<int>& skins() const { return m_skins; }
    const QStringList& browShapes() const { return m_browShapes; }
    const QStringList& hairColours() const { return m_hairColours; }
    // Beard shapes ship for men only, and there are far more of them than
    // there are brow shapes (a0…j2 rather than a0…k0).
    const QStringList& beardShapes() const { return m_beardShapes; }
    // Iris colour indices present (cm_iris<N>) and the shade suffixes each
    // ships (cm_iris<N>_c<SS>_bsm). Eight colours x two shades in retail.
    const QVector<int>& irisColours() const { return m_irisColours; }
    const QVector<int>& irisShades() const { return m_irisShades; }
    // A human name for iris index `n`, measured by decoding the shipped maps.
    static const char* irisName(int colourIdx);
    // v indices present for family 0 = gash, 1 = tato, 2 = fpnt.
    const QVector<int>& decoIds(int family) const;
    static const char* decoName(int family);

    // Asset paths (no extension) for one choice, or empty when absent.
    QString facePath(int wrinkle, int skin) const;
    // The same map for the gender the given head model belongs to. Both
    // genders ship a full face grid (avf0_type0_* and avm0_type0_*) and they
    // are NOT interchangeable — a man wearing the women's face map is the
    // "male models don't match" bug. `modelStem` is the head model's stem,
    // e.g. avm0_type3_def; anything unrecognised falls back to facePath().
    QString facePathFor(const QString& modelStem, int wrinkle, int skin) const;
    // The face's NORMAL map for one wrinkle set. These heads bind a 128x128
    // placeholder normal exactly as they bind a placeholder colour, so a face
    // that is retextured but not re-normalled comes out flat — the wrinkles
    // are IN the normal map, which is what makes a wrinkle set a wrinkle set.
    // One per wrinkle set and shared by every skin tone: <pre>_v<WW>_def_nrm.
    QString faceNormalPathFor(const QString& modelStem, int wrinkle) const;

    // ── THE SRMs THE GAME BINDS AND THIS TOOL WAS NOT ────────────────────
    //
    // Every runtime-substituted avatar material — the face, the hair, the
    // brows, the beard — ships with a FLAT PLACEHOLDER in its SpecularMap
    // slot, or with nothing in it at all, because the game binds the real map
    // when it picks the look. This tool substituted the base colour (and, on
    // the face, the normal), and left the specular slot as it found it: so
    // `uHasSrm` was 0 on every one of them and the shader fell back to its own
    // constants — occlusion 1, roughness 0.55, reflection 0. A face with no
    // occlusion and one flat roughness is exactly what "missing roughness map
    // and AO" looks like.
    //
    // The maps ship. Measured over the avatar's own texture folders:
    //
    //   face    avf0_type0_v00_def_srm, avm0_type0_v00_def_srm   one per gender
    //   brows   avm_ebrw0_<shape>_srm                            11
    //   beard   avm_berd0_<shape>_srm, avm_berd1_<shape>_srm     both layers
    //   hair    av[fm]_hair<0|1>_<style>_srm                     per style
    //
    // NONE of them carry a colour or a skin tone in the name: an SRM is
    // occlusion, roughness and reflection, and none of those changes because
    // the hair went from blonde to black. The face's is not even per wrinkle
    // set — one ships, v00, and the eight wrinkle NORMALS carry the difference.
    // Each of these probes anyway rather than hard-coding v00, so an install
    // that ships more is not ignored.
    QString faceSpecularPathFor(const QString& modelStem, int wrinkle) const;
    QString browSpecularPath(int shapeIdx) const;
    QString beardSpecularPath(int shapeIdx) const;
    // NO beardSkinSpecularPath. The berd1 "hair on skin" layer has no material
    // of its own — it is composited into the face's base map as an overlay —
    // so there is nothing to bind its 31 shipped _srm to. Named here so the
    // next person to notice those files does not go looking for the caller.
    QString hairSpecularPathFor(const QString& modelStem,
                                int onlyFamily = -1) const;
    QString browPath(int shapeIdx, int colourIdx) const;
    QString beardPath(int shapeIdx, int colourIdx) const;
    // The preset table stores a beard as a family (0-4) and a variant (0-2),
    // which is a different index space from beardShapes(). The shipped names
    // are <letter><digit> — a0…a3, b0…b2, … — so family picks the letter and
    // variant the digit. Returns the beardShapes() index, or -1 when this
    // install ships no such beard. Never guesses a nearby one.
    int beardIndexOf(int family, int variant) const;
    // The SKIN side of a beard. A beard is two layers, not one: the browser
    // already had the card atlas that goes on the beard MESH (beardPath), and
    // this is the stubble painted straight onto the face, in the face's own UV
    // layout, which the game composites over the face map the way it does a
    // scar. avm_berd0_* is the first, avm_berd1_* the second — same fifteen
    // shape names in both. Empty when this install ships no skin layer.
    QString beardSkinPath(int shapeIdx, int colourIdx) const;
    // The hair map for one style and colour. `onlyFamily` restricts the search
    // to hair0 or hair1; -1 takes whichever answers, hair0 first.
    QString hairPath(const QString& modelStem, int colourIdx,
                     int onlyFamily = -1) const;
    // The HAIRLINE: that hairstyle painted in the FACE's own UV layout, to be
    // laid over the face map — the scalp and stubble under the hair cards, and
    // the whole of a style that ships no mesh.
    //
    // Two families ship per style and they are not interchangeable. Measured
    // across the shipped sets: hair0 carries `dtm` and `trm` (the detail and
    // translucency a hair-card shader wants) and hair1 carries `nrm` and `srm`
    // with no `dtm`/`trm` at all — the same split as berd0 (the beard's card
    // atlas) against berd1 (the beard painted on the face), which this browser
    // already composites. Without it a hairstyle is cards floating over a bald
    // scalp, and a preset whose hairstyle has no mesh has no hair whatsoever.
    QString hairSkinPath(const QString& modelStem, int colourIdx) const;
    // ANY hair map in this colour, whichever style ships one. For a colour
    // chip the style does not matter and the caller has no model to name.
    QString anyHairPath(int colourIdx) const;
    QString decoPath(int family, int id, int skin) const;
    // The shared body skin for one tone, or empty when this install has none.
    // PER GENDER: the men and the women have separate five-tone sets
    // (avm0_body0_def_c00…c04 and avf0_body0_def_c00…c04), and picking one for
    // everybody put the women's skin on every bare male limb. `men` is the
    // SUBJECT's gender, taken from AvatarLook, not guessed from the part name.
    // Falls back to the other gender's set when this install ships only one.
    QString bodyPath(int skin, bool men) const;
    // The iris map for one colour and shade, or empty. These do NOT live under
    // the avatar roots — they are shared human eye art in
    // /Assets/{ssd,tpp}/common_source/chara/human/Pictures — so they are
    // resolved against their own root list.
    QString irisPath(int colourIdx, int shade) const;

private:
    AvatarTextures() = default;
    void build();
    // First root under which `name` resolves to an indexed .ftex, or empty.
    QString resolve(const QString& dir, const QString& name) const;

    QStringList m_roots;         // asset roots that answered, in probe order
    QString m_facePrefix;        // "avf0_type0"
    QStringList m_facePrefixes;  // every gender's face prefix that resolved
    QString m_browPrefix;        // "avm_ebrw0"
    QString m_beardPrefix;       // "avm_berd0" — the beard mesh's card atlas
    QString m_beardSkinPrefix;   // "avm_berd1" — the stubble on the face map
    int m_beardSkinShapes = 0;   // how many of m_beardShapes have that layer
    QStringList m_beardShapes;
    QString m_decoPrefix;        // "avm"
    QVector<int> m_wrinkles, m_skins;
    QStringList m_browShapes, m_hairColours;
    QVector<int> m_gash, m_tato, m_fpnt;
    QString m_bodyPrefix;        // "avf0_body0_def" — the women's
    QString m_bodyPrefixMen;     // "avm0_body0_def" — the men's
    QVector<int> m_bodyTones;
    QStringList m_humanRoots;    // common_source roots that answered
    QVector<int> m_irisColours, m_irisShades;
    QString m_note;
};

}  // namespace fox
