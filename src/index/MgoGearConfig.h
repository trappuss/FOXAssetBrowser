// MgoGearConfig.h — MGO3's gear tables, read from the game's own config.
//
// /Assets/mgo/level_asset/config/GearConfig.lua is 416 KB of plain ASCII Lua
// and it is the authority for everything the avatar can wear. Nothing here is
// inferred from model names; every field below is a field in that file.
//
// The shape, measured:
//
//   MgoGearConfig.Accessories{ AccessaryConfig={ Male={Eyes={…}},
//                                                Female={Eyes={…}} } }
//   MgoGearConfig.Gears{ GearConfig={ Male={Headgear={…},Base={…},Chest={…}},
//                                     Female={…} } }
//   MgoGearConfig.Colors{ ColorConfig={ Primary={…}, Secondary={…} } }
//
// which is FOUR categories per gender — Accessory (Eyes), Headgear, Base and
// Chest — matching what the community equipment list says.
//
// BUT THE GEARS SECTION IS KEYED BY CLASS FIRST. GearConfig={Infil={Male=…,
// Female=…}, Recon={…}, Tech={…}} — so Headgear, Base and Chest each appear
// SIX times in the file (three classes x two genders), not twice, and the
// three class copies of a category overlap heavily. Measured, per male block:
//
//   Headgear 1: 24 hat + 5 inh + 1 reh + 1 teh + 1 ins  = 32
//   Headgear 2: 24 hat + 5 reh + 1 inh + 1 teh + 1 res  = 32
//   Headgear 3: 24 hat + 5 teh + 1 inh + 1 reh + 1 tes  = 32
//   Base 1/2/3:  2 inb + 2 ins + 2 cms  (reb/res, teb/tes) = 6 each
//
// — the same 24 hats in every Headgear block, each class's own suit and heads,
// and one head from each of the other two classes. The COMMON suit (cms) is a
// Base item and never appears in Headgear. Reading each block as its own
// category is what put three separate "Headgear" rows on the page.
//
// Measured over the shipped file, per gender, top-level records per block and
// then unique by item id across the three class blocks:
//
//              raw (3 blocks)   unique
//   Headgear        96             42
//   Base            18             14
//   Chest        27 M / 30 F    25 M / 26 F
//   Eyes             6              6   (one block; not class-keyed)
//   TOTAL       147 M / 150 F   87 M / 88 F
//
// So the categories are merged by slot and deduplicated by id, first record
// wins. 87 male / 88 female is what the avatar can actually wear.
//
// Each item carries an ID, a NameLangTag and DescLangTag (which resolve
// through the .lng2 catalogue to the in-game name), a Swatch (its icon, a
// real .ftex path), an Exclude list of item ids it cannot be worn with, and a
// Color block with its OWN palette:
//
//   Color={DefaultPrimary="com_c06", DefaultSecondary="inh_c00",
//          Primary={"com_c08",…}, Secondary={"inh_c00","inh_c01","inh_c02"}}
//
// So gear colour in MGO3 is per ITEM, two channels, from a per-item palette —
// not one colour for the whole character. The Colors section then gives every
// colour id its own swatch icon.
//
// THE ID SCHEME decodes to the model, which is what makes this usable:
//
//   in* Infiltrator   re* Recon    te* Enforcer   cm* Common
//   *h  head          *c  chest    *b  base       *s  suit
//   hat headgear      eye accessory
//
// A "suit" carries FullSuit=1 — the file's own marker, not something to read
// off the prefix. Eighteen records do, nine per gender: ins/res/tes _?00, _?01
// and _?02 for each of the three classes. Measured, and NOT what the prefix
// rule would have said:
//   *_?01 is listed under Headgear
//   *_?00 and *_?02 are listed under Base
//   cms (the common suit) carries no FullSuit and is listed only under Base
// and NO id in the whole file appears in more than one category — cross-slot
// overlap is exactly zero. So a suit is not one item sitting in three rows; it
// is a garment whose head piece and body piece are separate entries, and what
// makes wearing one clear the others is its Exclude list, which names them.
//
// fullSuit is therefore parsed and carried but not yet acted on: the rule it
// belongs to is the exclusion pass, which is not built.
//
// THE TABLE is the gender authority, not the id and certainly not the model
// name. Reading gender off the MODEL name is what put the woman's gear on the
// man's page — MGO names the female fit with a trailing _f and leaves the male
// fit bare, so a name-based rule cannot tell "male fit" from "shared".
//
// The id's own letter is not the authority either, and it is worth being exact
// about why: the Male table does hold only _m ids, but the FEMALE table holds
// eight records whose ID ends in _m —
//   hat_m05 hat_m06 hat_m07 hat_m08 hat_m09 hat_m11 hat_m12 hat_m14
// — which with her sixteen hat_f* ids make up the same twenty-four hats the
// man has. Those eight are hats shipped ONCE with no female re-fit, so on her
// page they name the plain model rather than a "_f" one. Whose list a record
// is in decides whose it is; the letter in the id decides which model file it
// names. See MgoGearItem::modelStems.
#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace fox {

struct MgoGearItem {
    QString id;            // "inh_m00"
    QString nameTag;       // "mgo_name_inh_m00" — resolves through NameCatalog
    // The item's UI icon, as an asset path with NO EXTENSION —
    // "/Assets/mgo/ui/texture/EquipIcon/gear/inh_m00_alp". GearConfig writes it
    // with ".ftex" on the end and IconCatalog::swatchForPath appends ".ftex"
    // itself, so the raw field asked the index for "…_alp.ftex.ftex" and found
    // nothing, every time. Stripped once here rather than at each call site.
    QString swatch;
    QString defaultPrimary, defaultSecondary;
    QStringList primary, secondary;   // this item's own colour palettes
    QStringList exclude;              // ids it cannot be worn with
    // FullSuit=1 in the record. NOT "occupies three slots at once" — measured,
    // no id in the file appears in more than one category; see the suit note at
    // the top. It marks a garment whose pieces are separate entries that
    // exclude each other, and it is parsed and carried but not yet acted on.
    bool fullSuit = false;
    // Set on the CHEST half of a two-piece garment: the id of the Base item it
    // belongs to. Measured: exactly three records carry it (cmc_f01→cms_f01,
    // cmc_f02→cms_f02, cmc_m01→cms_m02), each alongside
    // RevertToDefaultBase=1, and each shares its partner's NameLangTag. A
    // chest row with this set is not a separate item and must not appear in
    // the Chest list on its own.
    QString baseGearId;
    // Items this one cannot be worn WITHOUT — the mirror of `exclude`. The
    // two-piece halves name each other; the four suit helmets
    // (ins/res/tes_?01) name the suit body they belong to, one-directionally.
    QStringList must;
    // The best-guess model stem this item names — "inh_m00" is inh0_main0_def.
    // Empty when the id does not follow the scheme. This is
    // modelStems().value(0): the FIRST candidate, not the only one, and on the
    // female page whether it ends in "_f" depends on the id (see modelStems).
    // Prefer modelStems() at a call site that can check which candidate
    // actually exists.
    QString modelStem(bool female) const;

    // EVERY stem this item might name, best first — THE FALLBACK JOIN.
    //
    // The primary join is the game's own: every item id ships a fova pack,
    // /Assets/mgo/pack/player/fova/<id>.fpk, whose .fv2 names the exact
    // model in its external-file table, and PlayerCatalog::buildMgo resolves
    // through it wherever the install carries the packs. This function
    // reproduces that mapping as name rules — decoded from all of the item
    // .fv2s in the real install's mgo chunk — so a partial copy without the
    // packs still resolves what it can. Anchors, all measured:
    //   inh_f00 -> inh0_main0_def_f    eye_f00 -> gls0_main1_def_f
    //   rec_f07 -> rec7_main0_def_f    cms_f01 -> cmn0_main0_def_f
    //   hat_m21 -> hat21_main0_def     teb_f03 -> tcl0_main0_def_f (the BDU)
    // The id number is a catalogue number, not a model index: the Base
    // families map ids 00..02 to model 1 and id 03 to model 0, a suit's _?01
    // is its mask0/helm0 piece, and hat5/hat9/gls0 are main1. The .cpp holds
    // the full measured set.
    QStringList modelStems(bool female) const;
};

// One of the four categories, for one gender.
struct MgoGearCategory {
    QString slotId;        // "mgo_headgear"
    QString label;         // "Headgear"
    QVector<MgoGearItem> items;
};

class MgoGearConfig {
public:
    static const MgoGearConfig& instance();

    bool ok() const { return m_ok; }
    QString note() const { return m_note; }
    // Categories for one gender, in the game's own order.
    const QVector<MgoGearCategory>& categories(bool female) const
    {
        return female ? m_female : m_male;
    }
    // A colour id's swatch icon, from the Colors section — asset path, NO
    // extension, ready for IconCatalog::swatchForPath.
    QString colourSwatch(const QString& colourId) const
    {
        return m_colourSwatch.value(colourId);
    }
    // "Solid" or "Pattern", the game's own About={ColorType=…}. Measured over
    // the shipped file: 367 colour ids, 282 Solid and 85 Pattern, and NOT ONE
    // of them carries a NameLangTag — the game ships no display names for
    // colours at all, which is why the list can only ever show the id. The
    // type is the one real label there is, so it is shown beside it.
    QString colourType(const QString& colourId) const
    {
        return m_colourType.value(colourId);
    }
    // EVERY colour the file defines, not just the ones some item's palette
    // happens to reach: 367 are defined and 337 are referenced, and a report
    // about one of the other 30 must not come back "no such id".
    QList<QString> colourIds() const { return m_colourSwatch.keys(); }
    int colourCount() const { return m_colourSwatch.size(); }
    int solidCount() const { return m_solid; }
    int patternCount() const { return m_pattern; }
    // Defined with a swatch but NO ColorType. Zero on the shipped file; a
    // separate bucket rather than folded into "solid", so an edited config
    // cannot silently invent solids.
    int untypedColourCount() const
    {
        return m_colourSwatch.size() - m_solid - m_pattern;
    }

private:
    void build();
    bool m_ok = false;
    QString m_note;
    QVector<MgoGearCategory> m_male, m_female;
    QHash<QString, QString> m_colourSwatch;
    QHash<QString, QString> m_colourType;   // id -> "Solid" / "Pattern"
    int m_solid = 0, m_pattern = 0;         // counted once, at parse time
    const void* m_indexKey = nullptr;
    int m_indexCount = -1;
};

}  // namespace fox
