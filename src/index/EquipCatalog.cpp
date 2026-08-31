// EquipCatalog.cpp — see EquipCatalog.h.
#include "index/EquipCatalog.h"

#include <QRegularExpression>
#include <QtGlobal>
#include <algorithm>

#include "index/ArchiveIndex.h"
#include "index/NameCatalog.h"

namespace fox {
namespace {

// gunBasic column order, validated column by column against the chimera
// directory each resolved stem lives in — see the header.
const char* const kGunBasicColumns[] = {
    "receiver", "barrel", "magazine", "stock", "muzzle", "muzzleOption",
    "sight", "sight2", "underBarrel", "option", "option2",
};
constexpr int kGunBasicColumnCount = 11;

// The inclusion tables' partsType for ONE slot. The two-entry rows are the
// primary and secondary slot of a pair, and they are genuinely different
// questions — read straight off the shipped rows:
//
//   type 6  1742 ST  (Optics 1)      type 7   180 ST            (Optics 2)
//   type 8   319 LT  (Flashlight)    type 9   238 LS + 44 LT    (Laser Sight)
//
// Asking for both and uniting them — which is what this did — offered laser
// sights in the flashlight slot and backup-iron-sight-only parts in Optics 1,
// so the switch narrowed far less than the game does.
int partsTypeForSlot(const QString& slot)
{
    static const QHash<QString, QVector<int>> kTypes = {
        {QStringLiteral("barrel"), {1}},
        {QStringLiteral("magazine"), {2}},
        {QStringLiteral("stock"), {3}},
        {QStringLiteral("muzzle"), {4}},
        {QStringLiteral("muzzleOption"), {5}},
        {QStringLiteral("sight"), {6, 7}},
        {QStringLiteral("underBarrel"), {10}},
        {QStringLiteral("option"), {8, 9}},
    };
    const QString base = EquipCatalog::baseSlot(slot);
    const QVector<int> v = kTypes.value(base);
    if (v.isEmpty()) return -1;
    if (slot == base) return v.first();
    return v.size() > 1 ? v[1] : -1;   // the secondary slot of a pair
}

QString unquote(const QString& s)
{
    QString out = s;
    if (out.startsWith(QLatin1Char('"'))) out.remove(0, 1);
    if (out.endsWith(QLatin1Char('"'))) out.chop(1);
    return out;
}


// The customize screen's class strip, in its own order. The left column is the
// raw category string EquipDevelopConstSetting.lua uses; the right is what the
// screen calls it. Every one of the eleven categories the shipped table
// carries is listed, so nothing falls through to the raw name in practice.
struct ClassRow { const char* category; const char* display; };
const ClassRow kClassOrder[] = {
    {"Handgun", "Handgun"},
    {"Submachinegun", "Submachine Gun"},
    {"Assault", "Assault Rifle"},
    {"Shotgun", "Shotgun"},
    {"GrenadeLauncher", "Grenade Launcher"},
    {"Sniper", "Sniper Rifle"},
    {"Machinegun", "Machine Gun"},
    {"Missile", "Rocket Launcher"},
    {"WalkerGear", "Walker Gear"},
    {"Quiet", "Quiet's Weapons"},
    {"SecurityGadgets", "Security Gadgets"},
};

}  // namespace

QString WeaponPreset::stemFor(const QString& slot) const
{
    for (const auto& p : parts)
        if (p.first == slot) return p.second;
    return {};
}

QString WeaponPreset::label() const
{
    const QString base = name.isEmpty() ? wpId : name;
    return grade > 0 ? QStringLiteral("%1 · Grade %2").arg(base).arg(grade) : base;
}

QStringList EquipCatalog::slotOrder()
{
    QStringList out;
    for (int i = 0; i < kGunBasicColumnCount; ++i)
        out << QLatin1String(kGunBasicColumns[i]);
    return out;
}

QString EquipCatalog::baseSlot(const QString& slot)
{
    if (slot.endsWith(QLatin1Char('2'))) {
        const QString base = slot.left(slot.size() - 1);
        if (base == QLatin1String("sight") || base == QLatin1String("option"))
            return base;
    }
    return slot;
}

namespace {
void logOnce(const EquipCatalog& c)
{
    if (c.hasPresets() || c.hasCompatibility())
        qInfo("equip: %s", qUtf8Printable(c.describe()));
    else
        qInfo("equip: no development data (EquipDevelopConstSetting.lua, "
              "EquipParameters.lua and WeaponPartsCombinationSettings.lua are "
              "not in the configured folders) — no game presets, no "
              "compatibility filtering");
}
}  // namespace

const EquipCatalog& EquipCatalog::instance()
{
    static EquipCatalog cache;
    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    if (cache.m_indexKey == files.constData() && cache.m_indexCount == files.size())
        return cache;
    cache.m_indexKey = files.constData();
    cache.m_indexCount = files.size();
    cache.build();
    logOnce(cache);
    return cache;
}

void EquipCatalog::build()
{
    m_dev.clear();
    m_loadouts.clear();
    m_grades.clear();
    m_presets.clear();
    m_usedSlots.clear();
    m_byFamily.clear();
    m_byReceiver.clear();
    m_byBarrel.clear();
    m_shipped.clear();
    m_shippedSlots.clear();
    m_suitNames.clear();
    m_suitIcons.clear();
    m_camoNames.clear();
    m_gearNames.clear();

    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    // Each table is read ONCE. A full install carries these luas in more than
    // one archive (00.dat and tpp_chunk0.dat both have them) at equal mount
    // priority, so neither is marked shadowed — parsing both appended every
    // development row twice and doubled the preset list.
    bool haveDev = false, havePar = false, haveCmb = false;
    for (int i = 0; i < files.size(); ++i) {
        const IndexedFile& f = files[i];
        if (!f.named || f.shadowed) continue;
        const bool dev = f.path.endsWith(QLatin1String("EquipDevelopConstSetting.lua"),
                                         Qt::CaseInsensitive);
        const bool par = f.path.endsWith(QLatin1String("EquipParameters.lua"),
                                         Qt::CaseInsensitive);
        const bool cmb = f.path.endsWith(
            QLatin1String("WeaponPartsCombinationSettings.lua"), Qt::CaseInsensitive);
        if ((!dev || haveDev) && (!par || havePar) && (!cmb || haveCmb)) continue;
        const QByteArray d = index.readFile(f);
        if (d.isEmpty()) continue;
        if (dev) { parseDevList(d); haveDev = true; }
        else if (par) { parseGunBasic(d); havePar = true; }
        else { parseCombinations(d); haveCmb = true; }
    }

    // Join: development row → loadout → model stems.
    const NameCatalog& names = NameCatalog::instance();
    for (const DevRow& r : m_dev) {
        const auto lit = m_loadouts.constFind(r.wpId);
        if (lit == m_loadouts.constEnd()) continue;
        WeaponPreset p;
        p.wpId = r.wpId;
        p.devId = r.devId;
        p.prereqDevId = r.prereq;
        p.category = r.category;
        p.grade = m_grades.value(r.wpId, 0);
        p.name = names.textForLabel(r.nameLabel);
        const QStringList& ids = lit.value();
        for (int c = 0; c < ids.size() && c < kGunBasicColumnCount; ++c) {
            const QString id = ids[c];
            if (id.isEmpty() || id.endsWith(QLatin1String("_None"))) continue;
            const QString stem = names.stemForPartId(id);
            if (stem.isEmpty()) continue;
            p.parts.append({QLatin1String(kGunBasicColumns[c]), stem});
        }
        if (p.parts.isEmpty()) continue;
        for (const auto& sp : p.parts) m_usedSlots.insert(sp.first);
        m_presets.append(p);
    }
    std::sort(m_presets.begin(), m_presets.end(),
              [](const WeaponPreset& a, const WeaponPreset& b) {
                  if (a.name != b.name) return a.name < b.name;
                  if (a.grade != b.grade) return a.grade < b.grade;
                  return a.devId < b.devId;
              });
    for (int i = 0; i < m_presets.size(); ++i) {
        const QString rc = m_presets[i].stemFor(QStringLiteral("receiver"));
        if (rc.isEmpty()) continue;
        // The family id is the model prefix WeaponCatalog groups by: the token
        // before the first underscore of "ar00_main0_def".
        m_byFamily[rc.section(QLatin1Char('_'), 0, 0)].append(i);
        m_shippedSlots[rc];   // the receiver is known even with a bare build
        for (const auto& sp : m_presets[i].parts)
            if (sp.first != QLatin1String("receiver")) {
                m_shipped[rc][sp.first].insert(sp.second);
                m_shippedSlots[rc].insert(sp.first);
            }
    }
    buildNamed();
}

// Collapse the build list into the weapons the customize screen actually
// lists: one entry per NAME, one tier per distinct grade. m_presets is already
// sorted by name → grade → devId, so a single pass groups it.
void EquipCatalog::buildNamed()
{
    m_named.clear();
    QHash<QString, int> byName;
    for (int i = 0; i < m_presets.size(); ++i) {
        const WeaponPreset& p = m_presets[i];
        if (p.name.isEmpty()) continue;   // 3 of 435 never resolve a name
        int at = byName.value(p.name, -1);
        if (at < 0) {
            NamedWeapon w;
            w.name = p.name;
            w.category = p.category;
            w.className = classDisplayName(p.category);
            w.classOrder = classOrderOf(p.category);
            at = m_named.size();
            m_named.append(w);
            byName.insert(p.name, at);
        }
        NamedWeapon& w = m_named[at];
        // One row per grade. A numeric weapon id is the development list's own
        // build; the lettered ones ("West_thg_010") are alternates that ship at
        // the same grade, so they only take the tier if nothing else has.
        int slot = -1;
        for (int t = 0; t < w.tiers.size(); ++t)
            if (w.tiers[t].first == p.grade) { slot = t; break; }
        bool numeric = false;
        p.wpId.toInt(&numeric);
        if (slot < 0) {
            w.tiers.append({p.grade, i});
        } else if (numeric) {
            bool hadNumeric = false;
            m_presets[w.tiers[slot].second].wpId.toInt(&hadNumeric);
            if (!hadNumeric) w.tiers[slot].second = i;
        }
    }
    for (NamedWeapon& w : m_named)
        std::sort(w.tiers.begin(), w.tiers.end(),
                  [](const QPair<int, int>& a, const QPair<int, int>& b) {
                      return a.first < b.first;
                  });
    std::sort(m_named.begin(), m_named.end(),
              [](const NamedWeapon& a, const NamedWeapon& b) {
                  if (a.classOrder != b.classOrder) return a.classOrder < b.classOrder;
                  if (a.className != b.className) return a.className < b.className;
                  return a.name < b.name;
              });
}

QString EquipCatalog::classDisplayName(const QString& category)
{
    for (const ClassRow& r : kClassOrder)
        if (category == QLatin1String(r.category))
            return QString::fromLatin1(r.display);
    return category;
}

int EquipCatalog::classOrderOf(const QString& category)
{
    for (int i = 0; i < int(sizeof(kClassOrder) / sizeof(kClassOrder[0])); ++i)
        if (category == QLatin1String(kClassOrder[i].category)) return i;
    return 99;
}

void EquipCatalog::parseDevList(const QByteArray& lua)
{
    // RegCstDev{p00=1001,p01=TppEquip.EQP_WP_10102,
    //           p02=TppMbDev.EQP_DEV_TYPE_Handgun,p03=1e3,…,p06="name_wp_1000",…}
    static const QRegularExpression recRe{
        QStringLiteral("RegCstDev\\s*\\{([^}]*)\\}")};
    static const QRegularExpression idRe{QStringLiteral("p00\\s*=\\s*([\\d.e+]+)")};
    static const QRegularExpression eqRe{
        QStringLiteral("p01\\s*=\\s*TppEquip\\.EQP_WP_(\\w+)")};
    static const QRegularExpression catRe{
        QStringLiteral("p02\\s*=\\s*TppMbDev\\.EQP_DEV_TYPE_(\\w+)")};
    static const QRegularExpression preRe{QStringLiteral("p03\\s*=\\s*([\\d.e+]+)")};
    static const QRegularExpression nameRe{QStringLiteral("p06\\s*=\\s*(\"[^\"]*\")")};
    // p08 is the item's UI icon. For the Suit rows its stem is either a
    // subject id ("ui_st_sna5_alp") or a camouflage index
    // ("ui_st_sna0_c12_alp") — the two shapes the naming below reads.
    static const QRegularExpression iconRe{QStringLiteral("p08\\s*=\\s*\"([^\"]*)\"")};
    // "ui_st_sna5_alp" and, WITHOUT the trailing digit, "ui_st_dla_alp". The
    // digit used to be required, which quietly excluded the whole DLC wardrobe
    // — dla/dlb/dlc/dld/dle, the five suits the avatar actually wears — from
    // both the name table and the icon table.
    static const QRegularExpression suitRe{
        QStringLiteral("ui_st_([a-z]{3}\\d?)_alp$")};
    static const QRegularExpression camoRe{
        QStringLiteral("ui_st_[a-z]{3}\\d_c(\\d+)_alp$")};
    // Buddy gear: "ui_dd_ddg0_m3_alp" names ddg0_main3_def outright.
    static const QRegularExpression gearRe{
        QStringLiteral("ui_(?:dd|dh|dw)_([a-z]{3}\\d)_m(\\d+)_alp$")};
    // Quiet's outfits are named the same way but with the model's own suffix:
    // ui_qwp_suit_qui0_alp → qui0_main0_def, ui_qwp_suit_qui0_gld_alp →
    // qui0_main0_gld, ui_qwp_suit_qui5_alp → qui5_main0_def. All six resolve to
    // a model the buddy_quiet2_* packs carry.
    static const QRegularExpression quietRe{
        QStringLiteral("ui_qwp_suit_([a-z]{3}\\d)(?:_([a-z]+))?_alp$")};

    const QString text = QString::fromLatin1(lua);
    auto it = recRe.globalMatch(text);
    while (it.hasNext()) {
        const QString rec = it.next().captured(1);
        const QRegularExpressionMatch em = eqRe.match(rec);
        if (!em.hasMatch()) continue;   // suits, buddy gear, heli — not weapons
        DevRow r;
        r.wpId = em.captured(1);
        // The generator writes large integers in Lua exponent form ("7e4"), so
        // a plain toInt() would read 1e3 as 1 and silently break the chain.
        const QRegularExpressionMatch im = idRe.match(rec);
        if (im.hasMatch()) r.devId = int(im.captured(1).toDouble());
        const QRegularExpressionMatch pm = preRe.match(rec);
        if (pm.hasMatch()) r.prereq = int(pm.captured(1).toDouble());
        const QRegularExpressionMatch cm = catRe.match(rec);
        if (cm.hasMatch()) r.category = cm.captured(1);
        const QRegularExpressionMatch nm = nameRe.match(rec);
        if (nm.hasMatch()) r.nameLabel = unquote(nm.captured(1));
        m_dev.append(r);
    }

    // Second pass over the SAME records for the rows that are not weapons:
    // uniforms and camouflage. These have no gunBasic loadout, so they are not
    // presets — they are names for things the builder already lists.
    it = recRe.globalMatch(text);
    while (it.hasNext()) {
        const QString rec = it.next().captured(1);
        const QRegularExpressionMatch im = iconRe.match(rec);
        const QRegularExpressionMatch nm = nameRe.match(rec);
        if (!im.hasMatch() || !nm.hasMatch()) continue;
        const QString stem = im.captured(1).section(QLatin1Char('/'), -1);
        const QString label = unquote(nm.captured(1));
        const QRegularExpressionMatch sm = suitRe.match(stem);
        if (sm.hasMatch() && !m_suitNames.contains(sm.captured(1))) {
            m_suitNames.insert(sm.captured(1), label);
            // The icon travels with the name. It is the same record, and a
            // suit is not in the weapon-parts table that every other icon
            // comes from, so without this an outfit row can be named and
            // still have no picture.
            m_suitIcons.insert(sm.captured(1), im.captured(1));
            continue;
        }
        const QRegularExpressionMatch qm = quietRe.match(stem);
        if (qm.hasMatch()) {
            const QString suffix = qm.captured(2).isEmpty()
                ? QStringLiteral("def")
                : qm.captured(2);
            const QString model =
                QStringLiteral("%1_main0_%2").arg(qm.captured(1), suffix);
            if (!m_gearNames.contains(model)) m_gearNames.insert(model, label);
            continue;
        }
        const QRegularExpressionMatch gm = gearRe.match(stem);
        if (gm.hasMatch()) {
            const QString model = QStringLiteral("%1_main%2_def")
                                      .arg(gm.captured(1))
                                      .arg(gm.captured(2).toInt());
            if (!m_gearNames.contains(model)) m_gearNames.insert(model, label);
            continue;
        }
        if (qEnvironmentVariableIsSet("FOXAB_DUMP_DEVICONS"))
            qInfo("devicon: %s | %s", qUtf8Printable(stem),
                  qUtf8Printable(NameCatalog::instance().textForLabel(label)));
        const QRegularExpressionMatch cm = camoRe.match(stem);
        if (cm.hasMatch()) {
            const QString key =
                QStringLiteral("c%1").arg(cm.captured(1).toInt(), 2, 10,
                                          QLatin1Char('0'));
            if (!m_camoNames.contains(key)) m_camoNames.insert(key, label);
        }
    }
}

void EquipCatalog::parseGunBasic(const QByteArray& lua)
{
    const QString text = QString::fromLatin1(lua);
    const int at = text.indexOf(QLatin1String("gunBasic={"));
    if (at < 0) return;
    // Brace-match the table rather than regex it: the other tables in this file
    // (receiver, sight, bullet…) have the same row shape and would be swept up.
    int depth = 0, end = -1;
    const int from = at + 9;
    for (int i = from; i < text.size(); ++i) {
        if (text[i] == QLatin1Char('{')) ++depth;
        else if (text[i] == QLatin1Char('}')) {
            if (--depth == 0) { end = i; break; }
        }
    }
    if (end < 0) return;
    static const QRegularExpression rowRe{QStringLiteral("\\{([^{}]*)\\}")};
    auto it = rowRe.globalMatch(text.mid(from, end - from + 1));
    while (it.hasNext()) {
        const QStringList f = it.next().captured(1).split(QLatin1Char(','));
        // WP + eleven parts + grade. Anything else is a different table's row.
        if (f.size() != kGunBasicColumnCount + 2) continue;
        const QString wp = f[0].section(QLatin1Char('.'), -1);
        if (!wp.startsWith(QLatin1String("WP_"))) continue;
        QStringList ids;
        for (int c = 1; c <= kGunBasicColumnCount; ++c)
            ids << f[c].section(QLatin1Char('.'), -1).trimmed();
        const QString id = wp.mid(3);
        m_loadouts.insert(id, ids);
        m_grades.insert(id, f[kGunBasicColumnCount + 1].trimmed().toInt());
    }
}

void EquipCatalog::parseCombinations(const QByteArray& lua)
{
    // RegistPartsInclusionInfo…{receiverID={…}|barrelID={…}, partsType=N,
    //                           partsIds={…}}
    // The record ends in "}}" — the inner brace closes partsIds, the outer the
    // record. The capture KEEPS the inner one: without it partsIds={…} arrives
    // unterminated and the field regex below silently never matches.
    static const QRegularExpression recRe{
        QStringLiteral("RegistPartsInclusionInfo\\w*\\s*\\{(.*?\\})\\s*\\}")};
    static const QRegularExpression keyRe{
        QStringLiteral("(receiverID|barrelID)\\s*=\\s*\\{([^}]*)\\}")};
    static const QRegularExpression typeRe{QStringLiteral("partsType\\s*=\\s*(\\d+)")};
    static const QRegularExpression partsRe{
        QStringLiteral("partsIds\\s*=\\s*\\{([^}]*)\\}")};
    static const QRegularExpression idRe{QStringLiteral("TppEquip\\.(\\w+)")};

    const QString text = QString::fromLatin1(lua);
    auto it = recRe.globalMatch(text);
    while (it.hasNext()) {
        const QString rec = it.next().captured(1);
        const QRegularExpressionMatch km = keyRe.match(rec);
        const QRegularExpressionMatch tm = typeRe.match(rec);
        const QRegularExpressionMatch pm = partsRe.match(rec);
        if (!km.hasMatch() || !tm.hasMatch() || !pm.hasMatch()) continue;
        const int type = tm.captured(1).toInt();
        QSet<QString> allowed;
        auto pit = idRe.globalMatch(pm.captured(1));
        while (pit.hasNext()) allowed.insert(pit.next().captured(1));
        if (allowed.isEmpty()) continue;
        auto& table = km.captured(1) == QLatin1String("barrelID") ? m_byBarrel
                                                                  : m_byReceiver;
        auto kit = idRe.globalMatch(km.captured(2));
        while (kit.hasNext()) table[kit.next().captured(1)][type].unite(allowed);
    }
}

QSet<QString> EquipCatalog::compatibleStems(const QString& slot,
                                            const QString& receiverStem,
                                            const QString& barrelStem) const
{
    // No early-out on empty tables: `gather` already no-ops on them, and the
    // shipped-builds union below is a rule in its own right — an install with
    // the development list but no WeaponPartsCombinationSettings.lua still
    // knows what the game fits to this weapon.
    const NameCatalog& names = NameCatalog::instance();

    QSet<QString> ids;
    const auto gather = [&](const QHash<QString, QHash<int, QSet<QString>>>& table,
                            const QString& stem) {
        if (stem.isEmpty()) return;
        // One model stem can back several part ids (the same physical part at
        // different grades). Any of them permitting a part permits it here:
        // the builder is a viewer, so the permissive reading is the safe one.
        const int type = partsTypeForSlot(slot);
        if (type < 0) return;
        for (const QString& keyId : names.partIdsForStem(stem)) {
            const auto kit = table.constFind(keyId);
            if (kit == table.constEnd()) continue;
            const auto tit = kit.value().constFind(type);
            if (tit != kit.value().constEnd()) ids.unite(tit.value());
        }
    };
    gather(m_byReceiver, receiverStem);
    gather(m_byBarrel, barrelStem);
    // A slot kTypes does not map (partsTypeForSlot returns -1) gathers nothing
    // and falls through to the shipped union alone. That is only benign because
    // m_shipped is keyed on the gunBasic column names, so such a slot has no
    // entry there either and the result is empty — "no rule" — rather than the
    // strictest possible one.
    QSet<QString> stems;
    for (const QString& id : ids) {
        const QString stem = names.stemForPartId(id);
        if (!stem.isEmpty()) stems.insert(stem);
    }
    // Whatever the inclusion tables say, a part the game itself fits to this
    // receiver in one of its shipped builds is compatible with it. Measured:
    // 53 of 1860 shipped parts are absent from their own receiver's inclusion
    // list, so without this the filter would hide parts the game uses.
    //
    // This union used to sit behind an "inclusion table said nothing → give up"
    // early return, which is exactly backwards: silence there is where the
    // shipped builds are the ONLY rule we have. Roughly a third of usable slots
    // were left completely unfiltered because of it — a switch that looked like
    // it did nothing. Empty now means what it claims: nothing is known, so show
    // everything.
    const auto sit = m_shipped.constFind(receiverStem);
    if (sit != m_shipped.constEnd())
        stems.unite(sit.value().value(slot));
    return stems;
}

namespace {
// Both tables are keyed the way the ICON is named, and the two families are
// spelled differently: Snake's suits carry their number ("ui_st_sna5_alp", and
// the models are sna5_main0_def), the DLC suits do not ("ui_st_dla_alp", while
// the models are dla0_plym0_def). So a caller with a model stem in hand has a
// key that is right for one family and one digit too long for the other. Try
// it as given, then without its trailing digit — never the reverse, so "sna5"
// can never be answered by a "sna" entry that happens to exist.
QString suitLookup(const QHash<QString, QString>& table, const QString& key)
{
    const auto exact = table.constFind(key);
    if (exact != table.constEnd()) return exact.value();
    if (key.size() > 1 && key.back().isDigit())
        return table.value(key.left(key.size() - 1));
    return {};
}
}  // namespace

// The suit family's icon path, exactly as the development table names it —
// "/Assets/tpp/ui/texture/EquipIcon/suit/ui_st_dla_alp", no extension.
QString EquipCatalog::suitIcon(const QString& subjectId) const
{
    return suitLookup(m_suitIcons, subjectId);
}

QString EquipCatalog::suitName(const QString& subjectId) const
{
    const NameCatalog& names = NameCatalog::instance();
    return names.textForLabel(suitLookup(m_suitNames, subjectId));
}

QString EquipCatalog::camoIndexFromTexture(const QString& texturePath)
{
    // "…/dds3_main0_def_c05_bsm.ftex" → "c05". The camouflage index is stamped
    // on the texture the variation substitutes, which is the only place it can
    // be read from for a player FOVA table.
    static const QRegularExpression camoTexRe{QStringLiteral("_(c\\d+)_[a-z]+\\.")};
    const QRegularExpressionMatch m = camoTexRe.match(texturePath.toLower());
    return m.hasMatch() ? m.captured(1) : QString();
}

QString EquipCatalog::gearName(const QString& modelStem) const
{
    const NameCatalog& names = NameCatalog::instance();
    return names.textForLabel(m_gearNames.value(modelStem));
}

QString EquipCatalog::camoName(const QString& variation) const
{
    // The index appears bare ("c12") and suffixed on the shared vehicle and
    // weapon paint tables ("camo_c12", "scol_c03"). It does NOT appear as the
    // "vNN" of a player FOVA table: those are per-model variation slots, and
    // the camouflage they carry has to be read out of the table itself —
    // dds5_main0_ply_v00 substitutes dds3_main0_def_c01_bsm, v01 substitutes
    // c00, v03 substitutes c05. See camoIndexFromTexture().
    static const QRegularExpression tailRe{QStringLiteral("(^|_)c(\\d+)$")};
    const QRegularExpressionMatch m = tailRe.match(variation.toLower());
    if (!m.hasMatch()) return {};
    const NameCatalog& names = NameCatalog::instance();
    const QString key =
        QStringLiteral("c%1").arg(m.captured(2).toInt(), 2, 10, QLatin1Char('0'));
    return names.textForLabel(m_camoNames.value(key));
}

bool EquipCatalog::hasBuildsFor(const QString& receiverStem) const
{
    return m_shippedSlots.contains(receiverStem);
}

bool EquipCatalog::slotEverUsedOn(const QString& receiverStem,
                                  const QString& slot) const
{
    const auto it = m_shippedSlots.constFind(receiverStem);
    return it != m_shippedSlots.constEnd() && it.value().contains(slot);
}

bool EquipCatalog::knowsReceiver(const QString& receiverStem) const
{
    if (receiverStem.isEmpty() || m_byReceiver.isEmpty()) return false;
    const NameCatalog& names = NameCatalog::instance();
    for (const QString& id : names.partIdsForStem(receiverStem))
        if (m_byReceiver.contains(id)) return true;
    return false;
}

QVector<int> EquipCatalog::presetsForFamily(const QString& familyId) const
{
    return m_byFamily.value(familyId);
}

QString EquipCatalog::dumpInclusion() const
{
    QString out = QStringLiteral("table\tkey\tpartsType\tpartId\tstem\n");
    const NameCatalog& names = NameCatalog::instance();
    const auto emitTable = [&](const QString& which,
                               const QHash<QString, QHash<int, QSet<QString>>>& t) {
        QStringList keys = t.keys();
        std::sort(keys.begin(), keys.end());
        for (const QString& k : keys) {
            QList<int> types = t.value(k).keys();
            std::sort(types.begin(), types.end());
            for (int ty : types) {
                QStringList ids = QStringList(t.value(k).value(ty).values());
                std::sort(ids.begin(), ids.end());
                for (const QString& id : ids)
                    out += which + QLatin1Char('\t') + k + QLatin1Char('\t')
                         + QString::number(ty) + QLatin1Char('\t') + id
                         + QLatin1Char('\t') + names.stemForPartId(id)
                         + QLatin1Char('\n');
            }
        }
    };
    emitTable(QStringLiteral("receiver"), m_byReceiver);
    emitTable(QStringLiteral("barrel"), m_byBarrel);
    return out;
}

QString EquipCatalog::describe() const
{
    int named = 0;
    for (const WeaponPreset& p : m_presets)
        if (!p.name.isEmpty()) ++named;
    return QStringLiteral("%1 game weapon build(s) (%2 named, %3 famil%4), "
                          "compatibility rules for %5 receiver(s) and %6 barrel(s)")
        .arg(m_presets.size())
        .arg(named)
        .arg(m_byFamily.size())
        .arg(m_byFamily.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies"))
        .arg(m_byReceiver.size())
        .arg(m_byBarrel.size())
        + QStringLiteral(", %1 uniform, %2 camouflage and %3 buddy-gear name(s)")
              .arg(m_suitNames.size())
              .arg(m_camoNames.size())
              .arg(m_gearNames.size());
}

}  // namespace fox
