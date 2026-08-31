// EquipCatalog.h — the game's OWN weapon builds and part-compatibility rules.
//
// Three generated Lua tables in master\0\00.dat carry the whole development
// system, and joined together they answer both of the questions the weapon
// builder could previously only guess at.
//
//   EquipDevelopConstSetting.lua   RegCstDev{p00=devId, p01=EQP_WP_<id>,
//                                  p02=category, p03=prerequisiteDevId,
//                                  p06="name_wp_1000" (the name label), …}
//   EquipParameters.lua  gunBasic  {WP, RC, BA, AM, SK, MZ, MO, ST, ST, UD,
//                                   LT, LT, grade} — one row per built weapon
//   WeaponPartsCombinationSettings.lua
//                                  RegistPartsInclusionInfo*{receiverID|
//                                  barrelID={…}, partsType=N, partsIds={…}}
//
// Part ids become model stems through NameCatalog (WeaponPartsUiSetting), so:
//
//   "AM MRS-4" grade 3 → WP_30032 → RC ar00_main0_def + BA ba00_main1_def +
//   AM am00_main3_def + SK sk00_main0_def + MZ mz00_main0_def +
//   MO su00_main0_def + ST st17_main0_def + UD gp00_main0_def + LT fl03_main0_def
//
// Column order is not assumed: every gunBasic column was checked against the
// chimera directory its resolved stem actually lives in (receiver 475/475,
// barrel 263/263, magazine 323/323, stock 301/301, muzzle 180/180,
// muzzleOption 154/155, sight 288+78, underBarrel 121/143, option 128+143).
// 437 of 455 development weapons join to a loadout, 432 of those resolve to a
// name, and 2085 of 2086 resolvable part stems exist in the chimera packs.
//
// Absent tables are not an error — Ground Zeroes and Survive have none. Both
// halves degrade to empty and every caller must treat empty as "no rule",
// never as "nothing is allowed".
#pragma once
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace fox {

// One row of the game's development list, resolved to model stems.
struct WeaponPreset {
    QString name;        // "AM MRS-4" — the in-game display name
    QString category;    // "Assault", "Handgun", "Sniper", …
    int grade = 0;       // 1…11 (the star grades are the high end)
    int devId = 0;
    int prereqDevId = 0; // the build this one is developed from (0 = a root)
    QString wpId;        // "30032"
    // Slot name (a chimera directory, plus the secondary "sight2"/"option2")
    // → model stem. Ordered as the game lists them.
    QVector<QPair<QString, QString>> parts;

    QString stemFor(const QString& slot) const;
    // "AM MRS-4 · Grade 3"
    QString label() const;
};

class EquipCatalog {
public:
    static const EquipCatalog& instance();

    // One weapon as the game's customize screen lists it: the display name, the
    // class it is filed under, and one entry per STAR TIER — the distinct
    // development grades that name ships at. Measured on the shipped tables:
    // 435 builds collapse to 141 named weapons, and the WU S.PISTOL's five
    // tiers (grades 1…5) are exactly the five the customize screen offers.
    //
    // Grade is an ABSOLUTE development tier (1…11 across the whole tree), not a
    // per-weapon 1…N index: 62 weapons top out above grade 6 while no weapon
    // has more than 8 distinct grades, so the tiers of one weapon can skip.
    struct NamedWeapon {
        QString name;        // "WU S.PISTOL"
        QString category;    // the raw table category ("Handgun", "Assault")
        QString className;   // the in-game class name ("Assault Rifle")
        int classOrder = 99; // the order the customize screen's class strip uses
        // {grade, index into presets()}, ascending. Where the tables ship more
        // than one build at the same grade (40 name/grade pairs, 32 of them a
        // numeric/"West_" pair), the numeric one is the tier and the other
        // stays reachable through the full preset list.
        QVector<QPair<int, int>> tiers;
    };
    const QVector<NamedWeapon>& namedWeapons() const { return m_named; }
    // "Assault" → "Assault Rifle"; an unknown category is returned unchanged.
    static QString classDisplayName(const QString& category);
    // Where that class sits in the customize screen's strip; unknown = last.
    static int classOrderOf(const QString& category);

    // Every built weapon the game ships, sorted by name then grade.
    const QVector<WeaponPreset>& presets() const { return m_presets; }
    // The presets whose receiver stem starts with this family id ("ar00").
    QVector<int> presetsForFamily(const QString& familyId) const;

    // Which part stems the game allows in `slot` for this receiver (and, for
    // the barrel-keyed slots, this barrel). EMPTY MEANS "NO RULE": the tables
    // are missing, or this receiver has no entry, and the caller must show
    // everything rather than nothing.
    QSet<QString> compatibleStems(const QString& slot,
                                  const QString& receiverStem,
                                  const QString& barrelStem) const;

    // The in-game name of a uniform family ("sna5" → "BATTLE DRESS", "nin0" →
    // "CYBORG NINJA", "dla" → "FATIGUES (NS)"), or empty. The development
    // list's suit rows carry an icon path whose stem IS the family. Thirteen
    // resolve: eight named with a trailing digit, and the five DLC suits
    // without one. A model stem's family may be spelled either way, so both
    // are accepted — see suitLookup.
    QString suitName(const QString& subjectId) const;
    // The same family's icon path from the same table (no extension). NOTE for
    // anyone drawing it: unlike the weapon-parts icons this is a full-colour
    // photograph, not white line art — see IconCatalog::iconIsLineArt.
    QString suitIcon(const QString& subjectId) const;
    // The in-game name of a camouflage index ("c12" → "SQUARE"), or empty.
    // These indices are shared: they appear on Snake's uniforms, on D-Horse
    // and D-Dog gear and on vehicle paint.
    QString camoName(const QString& variation) const;
    // "…/dds3_main0_def_c05_bsm.ftex" → "c05", for feeding camoName() when the
    // variation's own name carries no index (the player FOVA tables).
    static QString camoIndexFromTexture(const QString& texturePath);
    // The in-game name of a buddy-gear model stem ("ddg0_main3_def" → "BATTLE
    // DRESS", "hrs3_main0_def" → "FURICORN", "mgm1_main0_def" → "D-WALKER").
    // The development list's icon path names the model directly:
    // ui_dd_ddg0_m3_alp → ddg0_main3_def. Twelve of the thirteen rows resolve
    // to a model that exists in the packs; the odd one out (hrs5_main0) is a
    // model the shipped pack does not carry.
    QString gearName(const QString& modelStem) const;
    int suitNameCount() const { return m_suitNames.size(); }
    int camoNameCount() const { return m_camoNames.size(); }

    bool hasPresets() const { return !m_presets.isEmpty(); }
    // Whether any shipped build actually fills this slot — the builder only
    // grows a second sight/option row when the game's own data uses one.
    bool slotIsUsed(const QString& slot) const { return m_usedSlots.contains(slot); }
    bool hasCompatibility() const { return !m_byReceiver.isEmpty(); }
    // Whether the inclusion tables know this receiver at all. The difference
    // matters: a receiver the tables have never heard of tells us NOTHING about
    // a slot, while a receiver they know with no entry for a slot is the game
    // saying that weapon cannot take that part — which is what the customize
    // screen draws a red cross over.
    bool knowsReceiver(const QString& receiverStem) const;

    // Whether the game ships any build on this receiver at all, and whether any
    // of them fills this exact slot. Together these are what the customize
    // screen draws its red cross from: a weapon the game builds, with a slot no
    // build of it ever uses, is a slot that weapon cannot take. Checked against
    // the shipped screens — the WU S.PISTOL crosses out Stock, Muzzle, both
    // Optics, Laser Sight and Underbarrel, and keeps Barrel, Magazine, Muzzle
    // Accessory and Flashlight, which is exactly what this reports.
    bool hasBuildsFor(const QString& receiverStem) const;
    bool slotEverUsedOn(const QString& receiverStem, const QString& slot) const;
    QString describe() const;
    // Data probe: the raw inclusion tables, one line per
    // key/partsType/partId, so a question about what a partsType actually
    // contains is answered by reading the game's own rows.
    QString dumpInclusion() const;

    // The slot names this catalogue can talk about, in the game's own order.
    static QStringList slotOrder();
    // "sight2" → "sight" (the catalogue directory a secondary slot draws from).
    static QString baseSlot(const QString& slot);

private:
    void build();
    void buildNamed();
    void parseDevList(const QByteArray& lua);
    void parseGunBasic(const QByteArray& lua);
    void parseCombinations(const QByteArray& lua);

    struct DevRow {
        int devId = 0;
        int prereq = 0;
        QString wpId;
        QString category;
        QString nameLabel;
    };

    const void* m_indexKey = nullptr;
    int m_indexCount = -1;

    QVector<DevRow> m_dev;
    // WP id → the eleven part ids of that build, in gunBasic column order.
    QHash<QString, QStringList> m_loadouts;
    QHash<QString, int> m_grades;

    QVector<WeaponPreset> m_presets;
    QVector<NamedWeapon> m_named;
    QSet<QString> m_usedSlots;
    QHash<QString, QVector<int>> m_byFamily;

    // key part id ("RC_30001" / "BA_30001") → partsType → allowed part ids
    QHash<QString, QHash<int, QSet<QString>>> m_byReceiver;
    QHash<QString, QHash<int, QSet<QString>>> m_byBarrel;
    // receiver stem → EXACT slot → the part stems the game's OWN builds fit to
    // it. The inclusion tables miss a few of these (53 of 1860 measured), and a
    // part the game ships on a weapon must never be filtered off it. Keyed on
    // the exact slot because "sight"/"sight2" and "option"/"option2" are
    // different questions in the game's tables too (partsType 6 vs 7, 8 vs 9).
    QHash<QString, QHash<QString, QSet<QString>>> m_shipped;
    // Same source, but keyed on the EXACT slot rather than the base one:
    // "sight2" and "option2" have to stay distinct here, because a sniper
    // rifle fills Laser Sight and never Flashlight.
    QHash<QString, QSet<QString>> m_shippedSlots;
    QHash<QString, QString> m_suitNames;   // "sna5" → "BATTLE DRESS"
    QHash<QString, QString> m_suitIcons;   // "sna5" → ".../ui_st_sna5_alp"
    QHash<QString, QString> m_camoNames;   // "c12"  → "SQUARE"
    QHash<QString, QString> m_gearNames;   // "ddg0_main3_def" → "BATTLE DRESS"
};

}  // namespace fox
