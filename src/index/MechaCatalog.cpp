// MechaCatalog.cpp — see MechaCatalog.h.
#include "index/MechaCatalog.h"

#include <QRegularExpression>
#include <algorithm>
#include <initializer_list>

#include "index/ArchiveIndex.h"
#include "index/BuildTimer.h"
#include "index/GameId.h"

namespace fox {
namespace {

// Body first — it is the host everything else seats on — then the rest in the
// order a person would build one. Slots the install lacks are dropped; slots
// not listed here are appended rather than discarded, so an unfamiliar vehicle
// still shows everything it has.
const char* const kSlotOrder[] = {
    "body", "head", "turret", "arm", "rotor", "weapon", "ammo", "shield",
    "glass", "light", "marking",
};

QString stemOf(const QString& path)
{
    return path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
}

// Directory → a readable name, only where the asset tree corroborates it.
// "uth" is the UTH-66 (its models are the helicopter in mis_com_helicopter),
// "mgm" is the D-Walker (the development list's WalkerGear rows carry the icon
// ui_dw_mgm1_m0). Everything else keeps its raw directory name rather than
// being given one I cannot back up.
QString groupNameFor(const QString& dir)
{
    // Each of these is corroborated by the pack that ships the models — the
    // game's own file naming, not a guess. veh_mc_ambulance holds amb0,
    // veh_mc_east_tnk holds mbt0, veh_mc_west_trc holds nmt0, and so on;
    // mis_com_sahelan holds mgs0. Codes with no such corroboration keep their
    // raw directory name.
    static const QHash<QString, QString> kNames = {
        {QStringLiteral("uth"), QStringLiteral("UTH-66 helicopter")},
        {QStringLiteral("mgm"), QStringLiteral("D-Walker")},
        {QStringLiteral("mgs"), QStringLiteral("Sahelanthropus")},
        {QStringLiteral("amb"), QStringLiteral("Ambulance")},
        {QStringLiteral("slv"), QStringLiteral("Light vehicle (East)")},
        {QStringLiteral("plv"), QStringLiteral("Light vehicle (West)")},
        {QStringLiteral("mbt"), QStringLiteral("Tank (East)")},
        {QStringLiteral("nbt"), QStringLiteral("Tank (West)")},
        {QStringLiteral("smt"), QStringLiteral("Truck (East)")},
        {QStringLiteral("nmt"), QStringLiteral("Truck (West)")},
        {QStringLiteral("sav"), QStringLiteral("Armoured vehicle (East)")},
        {QStringLiteral("wav"), QStringLiteral("Armoured vehicle (West)")},
        {QStringLiteral("scr"), QStringLiteral("Truck cargo (East)")},
        {QStringLiteral("crg"), QStringLiteral("Truck cargo (West)")},
        {QStringLiteral("hew"), QStringLiteral("Vehicle weapon")},
    };
    return kNames.value(dir, dir);
}

// Part-kind token → slot. Only tokens actually seen in the mecha tree are
// mapped; an unmapped one gets a slot of its own so nothing is hidden.
QString slotForKind(const QString& kind, const QString& dir)
{
    // Everything in /weapon/hew/ IS a weapon — hw01_main0_def is the tank's
    // main gun, not a tank. Without this its "main" token would file it as a
    // body and it would become a subject of its own with nothing to fit it to.
    if (dir == QLatin1String("hew")) return QStringLiteral("weapon");

    static const QHash<QString, QString> kMap = {
        {QStringLiteral("main"), QStringLiteral("body")},
        {QStringLiteral("head"), QStringLiteral("head")},
        {QStringLiteral("turt"), QStringLiteral("turret")},
        {QStringLiteral("rarm"), QStringLiteral("arm")},
        {QStringLiteral("arms"), QStringLiteral("arm")},
        {QStringLiteral("rotr"), QStringLiteral("rotor")},
        {QStringLiteral("mgun"), QStringLiteral("weapon")},
        {QStringLiteral("sgun"), QStringLiteral("weapon")},
        {QStringLiteral("towm"), QStringLiteral("weapon")},
        {QStringLiteral("famo"), QStringLiteral("weapon")},
        {QStringLiteral("fltn"), QStringLiteral("weapon")},
        {QStringLiteral("fmtw"), QStringLiteral("weapon")},
        {QStringLiteral("miss"), QStringLiteral("weapon")},
        {QStringLiteral("rckt"), QStringLiteral("weapon")},
        {QStringLiteral("wepn"), QStringLiteral("weapon")},
        {QStringLiteral("brlh"), QStringLiteral("ammo")},
        {QStringLiteral("ammo"), QStringLiteral("ammo")},
        {QStringLiteral("attc"), QStringLiteral("ammo")},
        {QStringLiteral("mcht"), QStringLiteral("ammo")},
        {QStringLiteral("shld"), QStringLiteral("shield")},
        {QStringLiteral("glas"), QStringLiteral("glass")},
        {QStringLiteral("glss"), QStringLiteral("glass")},
        {QStringLiteral("wind"), QStringLiteral("glass")},
        {QStringLiteral("logo"), QStringLiteral("marking")},
        {QStringLiteral("mark"), QStringLiteral("marking")},
    };
    return kMap.value(kind);
}

const CatalogPart* ownPartIn(const QHash<QString, QVector<CatalogPart>>& bySlot,
                             const QHash<QString, QString>& stemToDir,
                             const QString& subjectId, const QString& slot)
{
    const auto it = bySlot.constFind(slot);
    if (it == bySlot.constEnd()) return nullptr;
    for (const CatalogPart& p : it.value())
        if (stemToDir.value(p.displayName) == subjectId) return &p;
    return nullptr;
}

}  // namespace

QString MechaCatalog::subjectOf(const QString& stem)
{
    const QString head = stem.section(QLatin1Char('_'), 0, 0);
    if (head.size() < 4) return {};
    for (int i = 0; i < 3; ++i)
        if (!head[i].isLetter()) return {};
    for (int i = 3; i < head.size(); ++i)
        if (!head[i].isDigit()) return {};
    return head.left(3);
}

QString MechaCatalog::kindOf(const QString& stem)
{
    const QString second = stem.section(QLatin1Char('_'), 1, 1);
    int i = 0;
    while (i < second.size() && second[i].isLetter()) ++i;
    return i > 0 ? second.left(i) : QString();
}

QVector<AttachOption> MechaCatalog::attachPlanFor(const QString& slot)
{
    // Measured from the .fcnp files these very archives carry — the points a
    // model OFFERS are what a part can be seated on:
    //
    //   mgm1_main0_def  CNP_HEAD CNP_awp_l CNP_awp_r CNP_HP_L_A CNP_HP_R_A
    //                   CNP_SHLD_F CNP_SHLD_L CNP_SHLD_R CNP_CARA CNP_CPAN
    //                   CNP_light_l0 CNP_light_r0 CNP_ppos_a
    //   mgm1_rarm0_def  CNP_RIGHT_HAND CNP_MCHT
    //   uth0_main0_def  CNP_roter_f CNP_roter_b CNP_ppos_a/b/c
    //                   CNP_window_lmaindoor CNP_window_rmaindoor …
    //   uth0_wepn0_def  CNP_WEAPON_L_A CNP_WEAPON_L_B CNP_WEAPON_R_C
    //                   CNP_WEAPON_R_D
    //   uth0_arms0_def  CNP_awp_a CNP_ppos
    //
    // Both vehicles are covered by one table because the point names do not
    // collide. A slot with no entry here is loaded but left unseated and
    // reported — that is deliberate: an invented connect point puts the part
    // at the origin and looks like a rendering bug.
    const auto onHost = [](const QString& host,
                           std::initializer_list<const char*> cnps) {
        QVector<AttachOption> out;
        for (const char* c : cnps) out.append({host, QLatin1String(c)});
        return out;
    };
    const QString body;                       // "" = the chosen variant
    const QString arm = QStringLiteral("arm");
    const QString weapon = QStringLiteral("weapon");

    if (slot == QLatin1String("head"))
        return onHost(body, {"CNP_HEAD"});
    if (slot == QLatin1String("rotor"))
        return onHost(body, {"CNP_roter_f", "CNP_roter_b"});
    if (slot == QLatin1String("turret"))
        return onHost(body, {"CNP_TURRET"});
    if (slot == QLatin1String("arm"))
        return onHost(body, {"CNP_awp_l", "CNP_awp_r", "CNP_ppos_a",
                             "CNP_ppos_b", "CNP_ppos_c"});
    if (slot == QLatin1String("weapon"))
        // A weapon hangs off whatever carries it: the turret on a wheeled
        // armoured vehicle, the arm on the D-Walker and the helicopter,
        // otherwise the body's own hardpoints. Tanks mount theirs straight on
        // the hull (mbt0_main0_def carries CNP_awp_a and CNP_awp_b).
        return onHost(QStringLiteral("turret"), {"CNP_awp_a", "CNP_awp_b"})
             + onHost(arm, {"CNP_RIGHT_HAND", "CNP_awp_a", "CNP_MCHT"})
             + onHost(body, {"CNP_awp_a", "CNP_awp_b", "CNP_HP_L_A",
                             "CNP_HP_R_A", "CNP_WEAPON_L_A", "CNP_WEAPON_L_B",
                             "CNP_WEAPON_R_C", "CNP_WEAPON_R_D"});
    if (slot == QLatin1String("ammo"))
        return onHost(weapon, {"CNP_AMMO", "CNP_AMMO_2ND", "CNP_AMMO_A",
                               "CNP_AMMO_B", "CNP_MCHT", "CNP_WP"})
             + onHost(arm, {"CNP_MCHT"})
             + onHost(body, {"CNP_CARG", "CNP_DECK_A"});
    if (slot == QLatin1String("shield"))
        return onHost(body, {"CNP_SHLD_F", "CNP_SHLD_L", "CNP_SHLD_R"});
    if (slot == QLatin1String("glass"))
        // Every windscreen and window point seen across the fleet: the
        // helicopter names its doors, the trucks name four sides, the
        // ambulance numbers three panes.
        return onHost(body, {"CNP_FRONTGLASS", "CNP_LEFTGLASS",
                             "CNP_RIGHTGLASS", "CNP_REARGLASS", "CNP_window_a",
                             "CNP_WIND0", "CNP_WIND1", "CNP_WIND2",
                             "CNP_window_front_a", "CNP_window_lmaindoor",
                             "CNP_window_rmaindoor", "CNP_window_lsubdoor_b",
                             "CNP_window_rsubdoor_b"});
    return {};
}

const MechaCatalog& MechaCatalog::instance()
{
    static MechaCatalog cache;
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

void MechaCatalog::build()
{
    BuildTimer bt("mecha");
    m_slotNames.clear();
    m_bySlot.clear();
    m_variations.clear();
    m_subjects.clear();
    m_stemToDir.clear();
    m_globalVariations.clear();

    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    // "hew" is /weapon/, not /mecha/, but it holds the tanks' main guns —
    // veh_mc_east_tnk.fpk ships hw03 alongside mbt0 — and they are useless in
    // the weapon builder, which is driven by the chimera packs.
    static const QRegularExpression modelRe(
        QStringLiteral("/Assets/[^/]+/(?:mecha/([a-z]{3})|weapon/(hew))"
                       "/Scenes/([^/]+)\\.fmdl$"),
        QRegularExpression::CaseInsensitiveOption);

    for (int i = 0; i < files.size(); ++i) {
        const IndexedFile& f = files[i];
        if (!f.named || f.shadowed) continue;
        if (!GameFilter::instance().enabled(index.gameOf(f))) continue;
        if (f.path.endsWith(QLatin1String(".fv2"), Qt::CaseInsensitive)) {
            const QString stem = stemOf(f.path);
            const int cut = stem.lastIndexOf(QLatin1Char('_'));
            if (cut <= 0) continue;
            CatalogVariation v;
            v.name = stem.mid(cut + 1);
            v.path = f.path;
            v.fileIdx = i;
            if (stem.startsWith(QLatin1String("mfv_"))) {
                // The shared vehicle paint tables — see the header. Prefix the
                // name with which table it came from so camo and secondary
                // colour do not collapse into one another.
                v.name = stem.left(cut).mid(4) + QLatin1Char('_') + v.name;
                bool dup = false;
                for (const CatalogVariation& e : m_globalVariations)
                    if (e.name == v.name) { dup = true; break; }
                if (!dup) m_globalVariations.append(v);
                continue;
            }
            m_variations[stem.left(cut)].append(v);
            continue;
        }
        const QRegularExpressionMatch m = modelRe.match(f.path);
        if (!m.hasMatch()) continue;
        const QString dir = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
        const QString stem = m.captured(3);
        if (stem.endsWith(QLatin1String("_patched"))) continue;
        const QString kind = kindOf(stem);
        if (kind.isEmpty()) continue;
        QString slot = slotForKind(kind, dir);
        if (slot.isEmpty()) slot = kind;   // unmapped: its own row, never hidden
        m_stemToDir.insert(stem, dir.toLower());
        CatalogPart p;
        p.slot = slot;
        p.id = stem;
        p.displayName = stem;
        p.modelFileIdx = i;
        auto& list = m_bySlot[slot];
        bool dup = false;
        for (const CatalogPart& e : list)
            if (e.displayName == p.displayName) { dup = true; break; }
        if (!dup) list.append(p);
    }

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
                  [](const CatalogPart& a, const CatalogPart& b) {
                      return a.displayName < b.displayName;
                  });

    // Subjects = directories that have at least one body model. Grouping by
    // DIRECTORY rather than model prefix is what keeps the D-Walker whole: its
    // body is mgm1 and its weapons are mgm0.
    QHash<QString, CatalogSubject> byDir;
    for (const CatalogPart& b : m_bySlot.value(QStringLiteral("body"))) {
        const QString dir = m_stemToDir.value(b.displayName);
        if (dir.isEmpty()) continue;
        CatalogSubject& s = byDir[dir];
        s.id = dir;
        s.variants.append(b);
    }
    for (auto it = byDir.begin(); it != byDir.end(); ++it) {
        it->groupName = groupNameFor(it->id);
        for (const QString& slot : m_slotNames) {
            if (slot == QLatin1String("body")) continue;
            if (ownPartIn(m_bySlot, m_stemToDir, it->id, slot))
                ++it->ownPartCount;
        }
        m_subjects.append(*it);
    }
    std::sort(m_subjects.begin(), m_subjects.end(),
              [](const CatalogSubject& a, const CatalogSubject& b) {
                  if (a.groupName != b.groupName) return a.groupName < b.groupName;
                  return a.id < b.id;
              });
    bt.setNote(QStringLiteral("%1 subject(s), %2 slot(s)").arg(m_subjects.size()).arg(m_bySlot.size()));
}

QVector<CatalogPart> MechaCatalog::partsFor(const QString& slot) const
{
    return m_bySlot.value(slot);
}

QVector<CatalogVariation> MechaCatalog::variationsFor(const QString& modelStem) const
{
    QVector<CatalogVariation> out = m_variations.value(modelStem);
    out += m_globalVariations;
    return out;
}

BuilderSource MechaCatalog::builderSource() const
{
    BuilderSource src;
    src.subjectLabel = QStringLiteral("Vehicle");
    src.variantLabel = QStringLiteral("Body");
    src.emptyHint = QStringLiteral(
        "No vehicle models found in the configured game folders. TPP keeps "
        "them under /Assets/tpp/mecha/<id>/Scenes/, packed into "
        "/Assets/tpp/pack/mission2/common/mis_com_helicopter.fpk and "
        "/Assets/tpp/pack/buddy/walkergear/.");
    src.slotNames = m_slotNames;
    src.subjects = m_subjects;
    src.partsFor = [this](const QString& s) { return m_bySlot.value(s); };
    src.ownPartFor = [this](const QString& id, const QString& s) {
        return ownPartIn(m_bySlot, m_stemToDir, id, s);
    };
    src.variationsFor = [this](const QString& stem) { return variationsFor(stem); };
    src.attachPlanFor = [](const QString& s) { return attachPlanFor(s); };
    return src;
}

}  // namespace fox
