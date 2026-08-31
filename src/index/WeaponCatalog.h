// WeaponCatalog.h — the weapon customization catalogue, discovered from the
// archives rather than hardcoded.
//
// MGSV's weapon customization system is called "chimera" internally, and its
// slots are literally directories:
//
//   /Assets/tpp/pack/collectible/chimera/<slot>/<partId>_v00.fpk
//
// with `slot` being receiver, barrel, muzzle, muzzleOption, sight, stock,
// magazine, underBarrel or option (flashlights + lasers) in stock TPP. Because
// the slot list comes from the data, a game with different or extra slots — or
// a partial install — describes itself correctly instead of being forced into
// a list we guessed.
//
// Parts seat on the receiver at named connect points from its .fcnp, and the
// names line up with the slots (CNP_BARREL, CNP_MUZZLE_OPTION, CNP_AMMO,
// CNP_GUN_STOCK/CNP_STOCK, CNP_SIGHT_RECEIVER, CNP_SYSTEM_LIGHT/LASER,
// CNP_WEAPON_ATTACH), and they are not all on the receiver — attachPlanFor()
// gives the (host part, connect point) preference order per slot.
//
// Appearance variations (camouflage, paint) are .fv2 FOVA tables — see
// fox::FovaFile — matched to a weapon by file-name stem.
#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include "index/PartCatalog.h"

namespace fox {

using WeaponPart = CatalogPart;

// One weapon FAMILY — "ar02", "hg01" — which is what a person actually means
// by "a weapon". Its receivers are the in-game versions/tiers of that gun
// (hg01 ships five, sg02 four), and parts sharing the family prefix are that
// weapon's own parts rather than generic ones.
using WeaponFamily = CatalogSubject;

using WeaponVariation = CatalogVariation;

class WeaponCatalog {
public:
    // Built once per index generation; cheap to call repeatedly.
    static const WeaponCatalog& instance();

    bool isEmpty() const { return m_slotNames.isEmpty(); }
    // Slots in a sensible build order (receiver first), filtered to those that
    // actually have parts in this install.
    // NOTE: not slots() — "slots" is a Qt keyword macro.
    const QStringList& slotNames() const { return m_slotNames; }
    QVector<WeaponPart> partsFor(const QString& slot) const;
    // Weapon families that have at least one receiver, ordered by class then id.
    const QVector<WeaponFamily>& families() const { return m_families; }
    // Everything the Customize builder needs, in the shared vocabulary.
    fox::BuilderSource builderSource() const;
    // The same-family part for this slot, if the family has one (its "own"
    // barrel/sight/magazine rather than a generic one). Lowest-numbered wins.
    const WeaponPart* ownPartFor(const QString& familyId, const QString& slot) const;
    // "ar02_main0_def" → "ar02". Empty when the name does not fit the scheme.
    static QString familyOf(const QString& stem);
    // Variations whose file name begins with this model stem.
    QVector<WeaponVariation> variationsFor(const QString& modelStem) const;
    // Every model stem this catalogue holds variations for, sorted. A census
    // (--camodump) has to cover the tables an install actually ships, and on a
    // partial extract those exist while the chimera PACKS that group them into
    // subjects do not — the variation map is built from the .fv2 files
    // themselves and is populated either way.
    QStringList variationStems() const;

    // Connect-point names to try, in order, when seating a part of this slot.
    static QVector<AttachOption> attachPlanFor(const QString& slot);

private:
    void build();

    const void* m_indexKey = nullptr;
    int m_indexCount = -1;
    // The game toggles narrow this catalogue too, so a change to them has to
    // rebuild it exactly the way a rescan does.
    int m_filterGeneration = -1;
    QStringList m_slotNames;
    QHash<QString, QVector<WeaponPart>> m_bySlot;
    QHash<QString, QVector<WeaponVariation>> m_variations;
    // Weapon paint is authored ONCE for every gun, not per model:
    // /Assets/tpp/pack/collectible/fova/ carries wfv_scol_cNN.fv2 alongside the
    // per-model def/cam/clv tables, and the substitution is keyed on material
    // name, so one table covers every weapon.
    QVector<WeaponVariation> m_globalVariations;   // model stem → list
    QVector<WeaponFamily> m_families;
};

}  // namespace fox
