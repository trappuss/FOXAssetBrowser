// WeaponCatalog.cpp — see WeaponCatalog.h.
#include "index/WeaponCatalog.h"

#include <QRegularExpression>
#include <algorithm>
#include <climits>
#include <initializer_list>

#include "index/ArchiveIndex.h"
#include "index/BuildTimer.h"
#include "index/GameId.h"

namespace fox {
namespace {

// Receiver first (it is the weapon body everything else hangs off), then the
// rest in the order a person would build one. Slots the install does not have
// are dropped; slots we have never seen are appended rather than discarded, so
// a game with new slots still lists them.
// The order and naming the game's own customize screen uses, top to bottom:
// Barrel, Magazine, Stock, Muzzle, Muzzle Accessory, Optics 1, Optics 2,
// Flashlight, Laser Sight, Underbarrel, Color. The receiver leads because it
// is the base everything else hangs off; "option"/"option2" are the game's
// Flashlight and Laser Sight, which is also what partsType 8 (LT) and 9 (LS)
// say they are.
const char* const kSlotOrder[] = {
    "receiver", "barrel", "magazine", "stock", "muzzle", "muzzleOption",
    "sight", "sight2", "option", "option2", "underBarrel",
};

// Class directory → a readable name. These are not guesses dressed up: each
// pairing is corroborated by the family prefixes filed under it — "asr" holds
// ar00-ar03, "hag" holds hg00-hg11, "snr" holds sr00-sr05, and so on, so the
// abbreviation and the model prefix agree. Codes without that corroboration
// (sas, put, enw, hew) are left as the raw directory name rather than invented.
QString classNameFor(const QString& dir)
{
    static const QHash<QString, QString> kNames = {
        {QStringLiteral("asr"), QStringLiteral("Assault rifle")},
        {QStringLiteral("hag"), QStringLiteral("Handgun")},
        {QStringLiteral("snr"), QStringLiteral("Sniper rifle")},
        {QStringLiteral("shg"), QStringLiteral("Shotgun")},
        {QStringLiteral("smg"), QStringLiteral("Submachine gun")},
        {QStringLiteral("mag"), QStringLiteral("Machine gun")},
        {QStringLiteral("mis"), QStringLiteral("Missile launcher")},
        {QStringLiteral("grl"), QStringLiteral("Grenade launcher")},
    };
    return kNames.value(dir, dir);
}

// Same rule as WeaponCatalog::ownPartFor, usable during build() before the
// member function's map is reachable through `this`.
const WeaponPart* ownPartForIn(const QHash<QString, QVector<WeaponPart>>& bySlot,
                               const QString& familyId, const QString& slot)
{
    const auto it = bySlot.constFind(slot);
    if (it == bySlot.constEnd()) return nullptr;
    for (const WeaponPart& p : it.value())
        if (WeaponCatalog::familyOf(p.displayName) == familyId) return &p;
    return nullptr;
}

QString stemOf(const QString& path)
{
    return path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
}

}  // namespace

const WeaponCatalog& WeaponCatalog::instance()
{
    static WeaponCatalog cache;
    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    const int gen = GameFilter::instance().generation();
    if (cache.m_indexKey == files.constData() && cache.m_indexCount == files.size()
        && cache.m_filterGeneration == gen)
        return cache;
    cache.m_indexKey = files.constData();
    cache.m_indexCount = files.size();
    cache.m_filterGeneration = gen;
    cache.build();
    return cache;
}

void WeaponCatalog::build()
{
    BuildTimer bt("weapons");
    m_slotNames.clear();
    m_bySlot.clear();
    m_variations.clear();
    m_globalVariations.clear();
    m_families.clear();

    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();

    // Pass 1: the packs. "…/collectible/chimera/<slot>/<id>.fpk" — capture the
    // slot from the path itself.
    static const QRegularExpression packRe(
        QStringLiteral("/pack/collectible/chimera/([^/]+)/([^/]+)\\.fpk$"),
        QRegularExpression::CaseInsensitiveOption);
    QHash<QString, QString> packStemToSlot;   // pack stem → slot
    for (int i = 0; i < files.size(); ++i) {
        if (!files[i].named || files[i].childIdx >= 0) continue;
        const QRegularExpressionMatch m = packRe.match(files[i].path);
        if (!m.hasMatch()) continue;
        packStemToSlot.insert(m.captured(2), m.captured(1));
    }
    QStringList packStems = packStemToSlot.keys();
    packStems.sort();

    // Pass 2: the models inside those packs, and the .fv2 variations. Both come
    // from container children the deep scan already indexed, so no archive I/O
    // happens here — a full game has hundreds of packs and opening each one
    // would stall the UI.
    for (int i = 0; i < files.size(); ++i) {
        const IndexedFile& f = files[i];
        if (!f.named) continue;
        // Skip the copy the game would NOT load: on a modded install the same
        // part exists twice, and offering both in a combo that shows only the
        // stem gives two identical-looking entries with different contents.
        if (f.shadowed) continue;
        if (!GameFilter::instance().enabled(index.gameOf(f))) continue;
        if (f.path.endsWith(QLatin1String(".fv2"), Qt::CaseInsensitive)) {
            const QString stem = stemOf(f.path);
            // "ar00_owep0_def_cam" → model "ar00_owep0_def", variation "cam".
            const int cut = stem.lastIndexOf(QLatin1Char('_'));
            if (cut <= 0) continue;
            WeaponVariation v;
            v.name = stem.mid(cut + 1);
            v.path = f.path;
            v.fileIdx = i;
            if (stem.startsWith(QLatin1String("wfv_"))) {
                // The shared weapon paint tables — see the header. The table
                // name is kept in front of the index so two of them cannot
                // collapse into one entry.
                v.name = stem.left(cut).mid(4) + QLatin1Char('_') + v.name;
                bool dup = false;
                for (const WeaponVariation& e : m_globalVariations)
                    if (e.name == v.name) { dup = true; break; }
                if (!dup) m_globalVariations.append(v);
                continue;
            }
            m_variations[stem.left(cut)].append(v);
            continue;
        }
        if (f.childIdx < 0) continue;
        if (!f.path.endsWith(QLatin1String(".fmdl"), Qt::CaseInsensitive)) continue;
        // Which pack is this child in? The parent's path is not carried on the
        // child, so match on the model stem against the pack stems: a pack
        // "hg07_main4_def_v00" carries "hg07_main4_def.fmdl".
        const QString modelStem = stemOf(f.path);
        // Match the model to its pack DETERMINISTICALLY. Iterating the hash
        // directly and taking the first hit made slot assignment depend on Qt's
        // per-process hash seed: with two packs matching one stem
        // ("ar00_owep0_def_v00" and "ar00_owep0_def_sight_v00") the same install
        // could file a part under a different slot on different runs. Sorted
        // keys plus "shortest remainder wins" makes it stable and picks the
        // pack named after the model rather than a longer sibling.
        QString slot;
        QString packId;
        int bestRest = INT_MAX;
        for (const QString& key : packStems) {
            if (!key.startsWith(modelStem)) continue;
            // "hg07_main4" must not match pack "hg07_main40_def_v00": what
            // follows the stem has to be a suffix, not more name.
            const QString rest = key.mid(modelStem.size());
            if (!rest.isEmpty() && !rest.startsWith(QLatin1Char('_'))) continue;
            if (rest.size() >= bestRest) continue;
            bestRest = rest.size();
            slot = packStemToSlot.value(key);
            packId = key;
        }
        if (slot.isEmpty()) continue;
        WeaponPart p;
        p.slot = slot;
        p.id = packId;
        p.displayName = modelStem;
        p.modelFileIdx = i;
        auto& list = m_bySlot[slot];
        bool dup = false;
        for (const WeaponPart& e : list)
            if (e.displayName == p.displayName) { dup = true; break; }
        if (!dup) list.append(p);
    }

    // Order the slots: known ones first in build order, then anything new.
    QStringList known;
    for (const char* s : kSlotOrder) {
        const QString slot = QString::fromLatin1(s);
        if (m_bySlot.contains(slot)) known.append(slot);
    }
    QStringList extra = m_bySlot.keys();
    std::sort(extra.begin(), extra.end());
    for (const QString& s : extra)
        if (!known.contains(s)) known.append(s);
    m_slotNames = known;

    for (auto it = m_bySlot.begin(); it != m_bySlot.end(); ++it)
        std::sort(it.value().begin(), it.value().end(),
                  [](const WeaponPart& a, const WeaponPart& b) {
                      return a.displayName < b.displayName;
                  });

    // ── Families ─────────────────────────────────────────────────────────────
    // A family is a weapon; its receivers are that weapon's versions. Starting
    // the UI from "receiver" means picking a part id before you can pick a gun,
    // which is backwards for anyone who thinks in weapons.
    QHash<QString, WeaponFamily> byFamily;
    for (const WeaponPart& r : m_bySlot.value(QStringLiteral("receiver"))) {
        const QString fam = familyOf(r.displayName);
        if (fam.isEmpty()) continue;
        WeaponFamily& f = byFamily[fam];
        f.id = fam;
        f.variants.append(r);
    }
    // Class comes from the part-definition path: /Assets/tpp/parts/weapon/<class>/…
    static const QRegularExpression clsRe(
        QStringLiteral("/parts/weapon/([^/]+)/([a-z]{2,3}\\d+)_"),
        QRegularExpression::CaseInsensitiveOption);
    for (int i = 0; i < files.size(); ++i) {
        if (!files[i].named) continue;
        const QRegularExpressionMatch m = clsRe.match(files[i].path);
        if (!m.hasMatch()) continue;
        const auto it = byFamily.find(m.captured(2));
        if (it == byFamily.end() || !it->groupName.isEmpty()) continue;
        it->groupName = classNameFor(m.captured(1));
    }
    for (auto it = byFamily.begin(); it != byFamily.end(); ++it) {
        if (it->groupName.isEmpty()) it->groupName = QStringLiteral("Other");
        for (const QString& slot : m_slotNames) {
            if (slot == QLatin1String("receiver")) continue;
            if (ownPartForIn(m_bySlot, it->id, slot)) ++it->ownPartCount;
        }
        m_families.append(*it);
    }
    std::sort(m_families.begin(), m_families.end(),
              [](const WeaponFamily& a, const WeaponFamily& b) {
                  if (a.groupName != b.groupName) return a.groupName < b.groupName;
                  return a.id < b.id;
              });
    bt.setNote(QStringLiteral("%1 slot(s), %2 famil(ies)").arg(m_bySlot.size()).arg(m_families.size()));
}

QString WeaponCatalog::familyOf(const QString& stem)
{
    // "ar02_main0_def" → "ar02": two or three letters then digits, up to the
    // first underscore. Anything else is not a chimera part name.
    const QString head = stem.section(QLatin1Char('_'), 0, 0);
    int i = 0;
    while (i < head.size() && head[i].isLetter()) ++i;
    if (i < 2 || i > 3 || i >= head.size()) return {};
    for (int j = i; j < head.size(); ++j)
        if (!head[j].isDigit()) return {};
    return head;
}

const WeaponPart* WeaponCatalog::ownPartFor(const QString& familyId,
                                            const QString& slot) const
{
    const auto it = m_bySlot.constFind(slot);
    if (it == m_bySlot.constEnd()) return nullptr;
    // Parts are already sorted by display name, so the first same-family hit is
    // the lowest-numbered one — the weapon's baseline part for that slot.
    for (const WeaponPart& p : it.value())
        if (familyOf(p.displayName) == familyId) return &p;
    return nullptr;
}

QVector<WeaponPart> WeaponCatalog::partsFor(const QString& slot) const
{
    return m_bySlot.value(slot);
}

QStringList WeaponCatalog::variationStems() const
{
    QStringList out = m_variations.keys();
    out.sort();
    return out;
}

QVector<WeaponVariation> WeaponCatalog::variationsFor(const QString& modelStem) const
{
    QVector<WeaponVariation> out = m_variations.value(modelStem);
    out += m_globalVariations;
    return out;
}

QVector<AttachOption> WeaponCatalog::attachPlanFor(const QString& slot)
{
    // Measured, not guessed: dumping every weapon .fcnp in the archives shows
    // which part carries which connect point, and the answer is that the
    // receiver does NOT carry them all. For the MRS-4 assault rifle:
    //
    //   ar00_main0_def (receiver)  CNP_AMMO CNP_REAR_SIGHT CNP_EJECT CNP_STOCK
    //                              CNP_RAIL CNP_BARREL CNP_SIGHT_RECEIVER
    //                              CNP_SIGHT_BARREL
    //   ba00_main1_def (barrel)    CNP_FRONT_SIGHT CNP_LEFT_HAND
    //                              CNP_SYSTEM_LIGHT CNP_SYSTEM_LASER
    //                              CNP_SYSTEM_GRIP CNP_SYSTEM_BIPOD
    //                              CNP_MUZZLE_OPTION CNP_UNDER_BARREL
    //   mz00_main0_def (muzzle)    CNP_MUZZLE_FLASH
    //
    // so muzzles, suppressors, foregrips and lights hang off the BARREL, and a
    // suppressor hangs off the muzzle in front of it. Receiver fallbacks are
    // kept because handguns and shotguns carry some of these points
    // themselves — the list is tried in order against what each host has.
    const auto onHost = [](const QString& host, std::initializer_list<const char*> cnps) {
        QVector<AttachOption> out;
        for (const char* c : cnps) out.append({host, QLatin1String(c)});
        return out;
    };
    const QString barrel = QStringLiteral("barrel");
    const QString muzzle = QStringLiteral("muzzle");

    if (slot == QLatin1String("barrel"))
        return onHost(QString(), {"CNP_BARREL"});
    if (slot == QLatin1String("stock"))
        return onHost(QString(), {"CNP_STOCK", "CNP_GUN_STOCK"});
    if (slot == QLatin1String("magazine"))
        return onHost(QString(), {"CNP_AMMO", "CNP_AMMO_2ND", "CNP_AMO"});
    if (slot == QLatin1String("sight"))
        return onHost(QString(), {"CNP_SIGHT_RECEIVER", "CNP_SIGHT_BARREL",
                                  "CNP_REAR_SIGHT", "CNP_RAIL"})
             + onHost(barrel, {"CNP_FRONT_SIGHT"});
    if (slot == QLatin1String("muzzle"))
        return onHost(barrel, {"CNP_MUZZLE_OPTION", "CNP_MUZZLE_FLASH"})
             + onHost(QString(), {"CNP_MUZZLE_OPTION", "CNP_MUZZLE_FLASH"});
    if (slot == QLatin1String("muzzleOption"))
        return onHost(muzzle, {"CNP_MUZZLE_FLASH"})
             + onHost(barrel, {"CNP_MUZZLE_OPTION", "CNP_MUZZLE_FLASH"})
             + onHost(QString(), {"CNP_MUZZLE_OPTION", "CNP_MUZZLE_FLASH"});
    if (slot == QLatin1String("underBarrel"))
        return onHost(barrel, {"CNP_UNDER_BARREL", "CNP_SYSTEM_GRIP"})
             + onHost(QString(), {"CNP_UNDER_BARREL", "CNP_WEAPON_ATTACH",
                                  "CNP_RAIL"});
    if (slot == QLatin1String("option"))
        return onHost(barrel, {"CNP_SYSTEM_LIGHT", "CNP_SYSTEM_LASER",
                               "CNP_SYSTEM_GRIP"})
             + onHost(QString(), {"CNP_SYSTEM_LIGHT", "CNP_SYSTEM_LASER",
                                  "CNP_RAIL"});
    return {};
}

BuilderSource WeaponCatalog::builderSource() const
{
    BuilderSource src;
    src.subjectLabel = QStringLiteral("Weapon");
    src.variantLabel = QStringLiteral("Version");
    src.emptyHint = QStringLiteral(
        "No weapon customization packs found in the configured game folders. "
        "TPP keeps them under /Assets/tpp/pack/collectible/chimera/<slot>/.");
    src.slotNames = m_slotNames;
    src.subjects = m_families;
    src.partsFor = [this](const QString& s) { return partsFor(s); };
    src.ownPartFor = [this](const QString& id, const QString& s) {
        return ownPartFor(id, s);
    };
    src.variationsFor = [this](const QString& stem) { return variationsFor(stem); };
    src.attachPlanFor = [](const QString& s) { return attachPlanFor(s); };
    return src;
}

}  // namespace fox
