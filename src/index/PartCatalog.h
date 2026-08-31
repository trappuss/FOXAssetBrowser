// PartCatalog.h — the vocabulary the Customize builder speaks, shared by every
// category.
//
// Weapons, characters, buddies and vehicles are assembled the same way: pick a
// SUBJECT (a weapon family, a character), pick which VARIANT of it, then fill a
// fixed set of SLOTS with parts, optionally applying an appearance VARIATION.
// Only the data differs. Keeping one vocabulary means the panel, the assembly
// and the preset code exist once instead of once per category — the same reason
// export menus live in one shared builder.
#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace fox {

struct CatalogPart {
    QString slot;          // "receiver", "barrel" / "body", "face", …
    QString id;            // pack or asset stem
    QString displayName;   // what the combo shows
    int modelFileIdx = -1; // ArchiveIndex file index of the .fmdl
    // Which of the game's own numbered presets this row IS, when the slot is
    // driven by a preset table rather than by one model per row (Survive's
    // avatar face list: 28 presets over 7 head models). -1 = not a preset.
    int presetIndex = -1;
    // MGO gear only, empty everywhere else: which GearConfig.lua item this
    // row IS, and the item ids the game says it cannot be worn with. The
    // Exclude lists are cross-slot (a suit body excludes chest garments and
    // open-face headgear) and NOT symmetric in the shipped data — inc_m00
    // excludes cms_m01 while cms_m01's own list names no inc id — so an
    // enforcement pass has to check both directions itself.
    QString gearId;
    QStringList gearExclude;
    // The mirror of gearExclude: items this one cannot be worn WITHOUT. The
    // four suit helmets name the suit body they belong to, one-directionally
    // (ins_?01 requires ins_?00 and not the other way round), so an
    // enforcement pass follows it in one direction only.
    QStringList gearMust;
    // The item's own UI icon, as an asset path with no extension. The game
    // ships one per gear item and the browser was showing none of them: the
    // field was parsed out of GearConfig.lua and then read by nothing.
    QString gearIcon;
    // A TWO-PIECE GARMENT. Three of MGO's items are one garment written as two
    // GearConfig rows — a Base row and a Chest row that names it in
    // BaseGearID, shares its NameLangTag and names it in Must. The chest row
    // is folded into this one rather than listed separately, so the garment is
    // chosen once, from the slot the game's own BaseGearID points at, and both
    // models are equipped together. Each half keeps its OWN colour channels,
    // which is why one of these ends up with two dye rows and another with
    // three.
    QString companionGearId;
    int companionFileIdx = -1;
    // HAIR UNDER A HAT. Every MGO hairstyle ships twice — "<style>_v00" and
    // "<style>_v0_cov" — and the pair is the game's own answer to hair
    // clipping through headgear: it swaps the MODEL, it does not hide meshes.
    // (Measured, and worth recording because it is the obvious wrong guess:
    // of 807 shipped .fv2 tables only 8 carry a hide list and all 8 hide the
    // same one group on the male head; `invisibleMeshNames` appears in two
    // files in the whole set, both the avatar BODY, and no headgear item
    // carries one. There is no per-hat mesh-hiding rule in this data.)
    //
    // The two forms are ONE row, and this holds the other one. -1 when the
    // install ships only one of them, in which case nothing swaps.
    int coveredFileIdx = -1;
};

struct CatalogVariation {
    QString name;          // "cam", "clv", "v01" — the suffix after the stem
    QString path;
    int fileIdx = -1;
};

// Where a part seats: the connect point `cnp` on the part fitted in `hostSlot`
// (empty hostSlot = the subject's own variant — the receiver / body).
//
// A weapon is a CHAIN, not a star: a muzzle mounts on the barrel's
// CNP_MUZZLE_OPTION, a suppressor on the muzzle's CNP_MUZZLE_FLASH, a foregrip
// on the barrel's CNP_UNDER_BARREL. Seating everything on the receiver put
// three parts at the origin — the receiver simply has no connect point for
// them. Each option is tried in order against what the host part actually
// carries.
struct AttachOption {
    QString hostSlot;   // "" = the variant (receiver / body)
    QString cnp;
};

// One buildable thing: a weapon family, a character. Its `variants` are the
// bodies/receivers the game ships for it.
struct CatalogSubject {
    QString id;                    // "ar02", "sna0"
    QString groupName;             // "Assault rifle", "Snake" — for grouping
    // The name to SHOW when the subject has a real one ("Survivor — Female").
    // Empty falls back to the group-and-id label a weapon family gets.
    QString displayName;
    // The variant model carries the skeleton but is never drawn — see
    // PlayerSubject::baseIsSkeletonOnly.
    bool hiddenVariant = false;
    QVector<CatalogPart> variants;
    int ownPartCount = 0;          // slots this subject has its own part for
};

// What the builder panel needs from a category, so the panel itself is written
// once. Set when the category changes.
struct BuilderSource {
    QString subjectLabel;   // "Weapon" / "Character"
    QString variantLabel;   // "Version" / "Body"
    QString emptyHint;      // shown when the category found nothing
    QStringList slotNames;
    QVector<CatalogSubject> subjects;
    std::function<QVector<CatalogPart>(const QString& slot)> partsFor;
    std::function<const CatalogPart*(const QString& subjectId, const QString& slot)>
        ownPartFor;
    std::function<QVector<CatalogVariation>(const QString& stem)> variationsFor;
    std::function<QVector<AttachOption>(const QString& slot)> attachPlanFor;

    // ── Contextual categories ────────────────────────────────────────────────
    // A weapon family has the same eleven slots as every other weapon family.
    // A CHARACTER does not: Snake has a uniform, a head option and an arm; a
    // Survive survivor has eight slots; a Ground Zeroes model has none at all.
    // When these are set the panel rebuilds its rows for the chosen subject
    // instead of showing one fixed set, and asks for that subject's own parts —
    // which is what stops a male head being offered on a female body.
    std::function<QStringList(const QString& subjectId)> slotsForSubject;
    std::function<QVector<CatalogPart>(const QString& subjectId,
                                       const QString& slot)> partsForSubject;
    std::function<QString(const QString& subjectId, const QString& slot)>
        slotLabelFor;
    // One line about the chosen subject, shown under the panel.
    std::function<QString(const QString& subjectId)> subjectNote;
    // What the subject wears with nothing chosen: slot id → model file index.
    // A character's "empty" state is not nothing — it is whatever the game
    // dresses a new one in.
    std::function<QHash<QString, int>(const QString& subjectId)> defaultsFor;
    // True when this subject's variant model is skeleton-only.
    std::function<bool(const QString& subjectId)> variantHidden;
    // The bone (StrCode32) a part in this slot hangs from when its own root
    // bone is absent from the wearer's skeleton, or 0 when the slot has no
    // fallback. Some parts are authored against a bone the character does not
    // carry, and without this they land at the model origin — at the waist.
    std::function<quint32(const QString& subjectId, const QString& slot)>
        anchorBoneFor;
    // The connect point on that bone, when the game authors one for this slot.
    // Empty = the bone itself. See PlayerSlot::anchorCnp.
    std::function<QString(const QString& subjectId, const QString& slot)>
        anchorCnpFor;

    bool valid() const { return partsFor && ownPartFor && variationsFor; }
    bool contextual() const { return slotsForSubject && partsForSubject; }
};

}  // namespace fox
