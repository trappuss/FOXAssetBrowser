// PlayerCatalog.h — the characters you can actually CUSTOMIZE, and only those.
//
// The Character category used to list every humanoid in the archives, each with
// whatever body models happened to share its three-letter code. That is a model
// browser, not a customizer: most of those characters have one model and no
// choices, while the handful the games let you dress have a different set of
// slots each. This lists the player characters, gives each one the slots its
// own game gives it, and fills those slots only with parts that fit.
//
// Everything below is read off the shipped data, not assumed:
//
//   SURVIVE  /Assets/ssd/pack/player/ carries plparts_base_male and
//            plparts_base_female, and one FPK per part under fova/<slot>/:
//            head 59, arm 56, body 56, leg 56, chest_rig 40, up_armor 24,
//            hats 8, glasses 2. Every part name is <two letters><gender><n>:
//            hdf/hdm, arf/arm, bdf/bdm, lgf/lgm, uaf/uam, rgf/rgm — 28/31,
//            23/23, 23/23, 22/24, 12/12, 20/20 models measured. The third
//            letter IS the gender lock. Hats carry it as a "_f" suffix on the
//            female fit (hat13_main0_def / hat13_main0_def_f) and glasses as
//            eye_f04 / eye_m04.
//
//   MGO 3    Lives in its OWN pair of .dat files under <install>/mgo/, apart
//            from the TPP chunks, and both sets are read. The avatar's BODIES
//            are gender-locked by name: the DLC outfits each ship
//            <id>_plyf0_def and/or <id>_plym0_def, and several ship only one
//            (dlc0/dlc1/dle0/dle1 female, dla0/dla1/dlb0/dld0 male).
//
//            What lives under /Assets/tpp/ is The Phantom Pain's own avatar
//            editor and it is MALE ONLY — the game's pack is
//            plparts_avatar_man with no woman counterpart, there is one body
//            (avm0_body0_def), and the face and hair families are avm alone:
//            avm0_type0…7 and avm1_type0…7, two sets of eight. MGO's archives
//            carry the female half — avf0_type0…7, avf_hair_a0…d0, her own
//            body and deform table — so a gender takes its own set when the
//            install has one and falls back to the shared avm set, saying so,
//            when it does not. See buildMgo.
//
//   TPP      Snake's own customization is his uniform (sna0…sna9_mainN_def,
//            named by the development list — BATTLE DRESS, SNEAKING SUIT…),
//            his head option (snaN_faceN_cov) and his arm (snaN_armN_cov plus
//            the rocket arms snaN_rktN_cov). The Diamond Dogs player is a
//            separate, gender-split character: the game's own pack names are
//            plparts_dd_male / plparts_dd_female and plparts_dd[mf]_battledress
//            / _parasite / _venom / _swimwear.
//
//   GZ       Ground Zeroes ships Snake as complete models with no part slots,
//            so he gets a body list and nothing else.
#pragma once
#include <QHash>
#include <QString>
#include <QVector>

#include "index/GameId.h"
#include "index/PartCatalog.h"

namespace fox {

enum class Gender : quint8 { Any = 0, Male, Female };
const char* genderName(Gender g);

// One customization slot of one player character.
struct PlayerSlot {
    QString id;            // "body", "face", "head", "arm", …
    QString label;         // "Outfit", "Face", "Headgear", …
    QVector<CatalogPart> parts;
    // The bone a part in this slot hangs from when its OWN root bone is not in
    // the wearer's skeleton — the StrCode32 of a real bone name, or 0 for "no
    // fallback". See anchorBoneFor() for why this is needed and how the bones
    // were identified.
    quint32 anchor = 0;
    // …and the CONNECT POINT on that bone, when the game authors one. A bone
    // is where a limb is; a connect point is where a THING GOES, and for
    // headgear those are 10cm apart: skl0's fcnp puts CNP_HEAD at
    // (0, 0.1029, 0.0108) on SKL_004_HEAD, which is the difference between a
    // cap on the crown and a cap over the mouth. Empty means "the bone is the
    // answer", which is what Survive's accessories want.
    QString anchorCnp;
};

// The StrCode32 of a bone name (the low 32 bits of the legacy name hash), which
// is what an FMDL stores instead of the name itself.
quint32 boneCode(const char* name);

// One character the game lets you dress.
struct PlayerSubject {
    QString id;            // "tpp_sna", "mgo_avatar_f", "ssd_m", …
    QString name;          // "Snake", "MGO Avatar — Female", "Survivor — Male"
    QString note;          // one line: where the slots came from
    GameId game = GameId::Unknown;
    Gender gender = Gender::Any;
    QVector<PlayerSlot> slotList;
    // Which slot is the character ITSELF — the one the Body combo shows rather
    // than a row. Snake's uniform is the character; a Survive survivor is a
    // base body with eight parts stacked on it, so there the Body combo is
    // that base and every slot is a row.
    QString variantSlot;
    // What the character wears with nothing chosen. The naked base body is not
    // that: Survive dresses a new survivor in a T-shirt, bare arms and plain
    // trousers, and the head comes from the head slot rather than the base.
    QHash<QString, QString> defaults;   // slot id → model stem
    // The model that is the character itself (base body / first outfit).
    int baseFileIdx = -1;
    QString baseStem;
    // True when the base model is only there to carry the skeleton and must
    // never be drawn. Survive's bsf0/bsm0 are the naked body the game animates
    // against; everything the player actually sees is a part stacked on it, so
    // showing it just clips through whatever is worn.
    bool baseIsSkeletonOnly = false;
    // True when the slots were READ OFF THE MODEL NAMES rather than named by
    // the game's own packs. The two kinds sit in separate groups in the
    // Character list, because "these are the slots the game gives Snake" and
    // "these are the parts this family ships, sorted by what they look like"
    // are different claims and the list should not make them sound alike.
    bool derived = false;
};

class PlayerCatalog {
public:
    static const PlayerCatalog& instance();

    const QVector<PlayerSubject>& subjects() const { return m_subjects; }
    const PlayerSubject* find(const QString& id) const;
    QString describe() const;
    // The Customize panel's Character category: player characters first, then
    // every other humanoid the archives carry, behind a divider.
    BuilderSource builderSource() const;

    // The gender a part name carries, from the naming rules above. Any means
    // the name makes no claim — such a part fits either character.
    static Gender genderOfPart(const QString& stem);

private:
    void build();
    void buildSurvive(const QHash<QString, int>& ssdModels,
                      const QHash<QString, int>& avatarModels);
    void buildMgo(const QHash<QString, int>& mgoModels,
                  const QHash<QString, int>& tppModels);
    void buildTpp(const QHash<QString, int>& models);
    // Everyone else The Phantom Pain ships as a dressable character — Quiet,
    // Ocelot, Miller, Huey, the Soviet and PF soldiers, the prisoners — built
    // from the part token in the model name rather than from a hand-written
    // list per character. See the comment on the definition for the measured
    // vocabulary that makes that possible.
    void buildTppCharacters(const QHash<QString, int>& models);
    void buildGz(const QHash<QString, int>& models);

    const void* m_indexKey = nullptr;
    int m_indexCount = -1;
    int m_filterGeneration = -1;
    QVector<PlayerSubject> m_subjects;
};

}  // namespace fox
