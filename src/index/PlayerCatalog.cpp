// PlayerCatalog.cpp — see PlayerCatalog.h.
#include "index/PlayerCatalog.h"

#include <QRegularExpression>
#include <QSet>
#include <algorithm>

#include "fox/FoxHash.h"
#include "fox/FovaFile.h"
#include "index/ArchiveIndex.h"
#include "index/MgoGearConfig.h"
#include "index/AvatarPresets.h"
#include "index/EquipCatalog.h"
#include "index/CharacterCatalog.h"
#include "index/NameCatalog.h"

namespace fox {
namespace {

// A model stem prettified for a combo when nothing better is known.
QString pretty(const QString& stem)
{
    QString out = stem;
    if (out.endsWith(QLatin1String("_def"))) out.chop(4);
    else if (out.endsWith(QLatin1String("_cov"))) out.chop(4);
    out.replace(QLatin1Char('_'), QLatin1Char(' '));
    return out;
}

// The in-game name when the data carries one, the tidied stem otherwise.
QString labelFor(const QString& stem)
{
    const QString real = NameCatalog::instance().nameFor(stem);
    if (!real.isEmpty()) return real;
    const QString suit =
        EquipCatalog::instance().suitName(stem.section(QLatin1Char('_'), 0, 0));
    if (!suit.isEmpty()) return suit;
    return pretty(stem);
}

CatalogPart makePart(const QString& slot, const QString& stem, int fileIdx)
{
    CatalogPart p;
    p.slot = slot;
    p.id = stem;
    p.displayName = stem;      // the panel prettifies; keep the asset name here
    p.modelFileIdx = fileIdx;
    return p;
}

// The order the rows are shown in: head to toe, then what is worn over it.
// The catalogues add slots in whatever order is convenient to build; a person
// reads a character from the face down.
int slotOrder(const QString& id)
{
    static const char* const kOrder[] = {
        // "body" is two different things: the OUTFIT on Snake (where it is
        // the Body combo, not a row) and the TORSO GARMENT in Survive (where
        // it is a row like any other). Ordering it with the other garments is
        // right in both readings. The MGO avatar has no "body" slot at all —
        // its Body combo is the animation base, which is not a slot.
        "head", "face", "hair", "beard", "brow", "head_equipment",
        "mgo_headgear", "band", "hood", "accessory", "sling", "lead",
        "hats", "glasses", "mgo_accessory", "horn", "body", "mgo_chest",
        "mgo_base", "torso", "chest_rig", "coat", "arm", "hand", "leg",
        "other",
    };
    for (int i = 0; i < int(sizeof(kOrder) / sizeof(kOrder[0])); ++i)
        if (id == QLatin1String(kOrder[i])) return i;
    return 99;
}

void addSlot(PlayerSubject& s, const QString& id, const QString& label,
             QVector<CatalogPart> parts)
{
    if (parts.isEmpty()) return;
    // Rows that ARE the game's own numbered grid keep the game's order. Sorting
    // them by model stem — which is what every other slot wants — would put
    // Face 2 and Face 4 next to each other because they happen to share a head.
    //
    // PRESETS FIRST. The old test read "if either side has a preset index,
    // compare the indices" — and a part with no preset carries -1, so -1 < 0
    // made every un-numbered head sort ahead of the whole numbered grid. On
    // the MGO male that put avm1_type0_def at the top of a list of 28 presets,
    // which is why the default face was not Face 1. It was also not a strict
    // weak ordering once two unnumbered parts met each other through the first
    // branch. Presets, in the game's order, then everything else by stem.
    const auto order = [](const CatalogPart& a, const CatalogPart& b) {
        const bool ap = a.presetIndex >= 0, bp = b.presetIndex >= 0;
        if (ap != bp) return ap;
        if (ap) return a.presetIndex < b.presetIndex;
        return a.id < b.id;
    };
    // Two prefixes can feed one slot (bd* and ua* are both Torso), so a second
    // call for the same id extends it instead of creating a duplicate row.
    for (PlayerSlot& existing : s.slotList)
        if (existing.id == id) {
            existing.parts += parts;
            std::sort(existing.parts.begin(), existing.parts.end(), order);
            return;
        }
    std::sort(parts.begin(), parts.end(), order);
    PlayerSlot slot;
    slot.id = id;
    slot.label = label;
    slot.parts = std::move(parts);
    s.slotList.append(slot);
    // Ties broken by ID. Every slot the generic character pass invents lands
    // at the same fallback order, and QHash hands them over in a different
    // sequence each run — which made the character dump differ between two
    // runs of the same build, and the dump is the project's gate.
    std::sort(s.slotList.begin(), s.slotList.end(),
              [](const PlayerSlot& a, const PlayerSlot& b) {
                  const int oa = slotOrder(a.id), ob = slotOrder(b.id);
                  return oa != ob ? oa < ob : a.id < b.id;
              });
}

}  // namespace

const char* genderName(Gender g)
{
    switch (g) {
        case Gender::Male: return "Male";
        case Gender::Female: return "Female";
        default: return "Any";
    }
}

// The gender a part name carries. Three separate conventions, each measured:
//   Survive parts  <two letters><f|m><digit>   arf0 / arm0, hdf17 / hdm17
//   Survive hats   a "_f" tail on the female fit  hat13_main0_def_f
//   Survive eyes   eye_f04 / eye_m04
//   MGO bodies     <id>_plyf0_def / <id>_plym0_def
//   avatar faces   avf… female, avm… male (Survive's presets are avf only)
// Anything else makes no claim and fits either character.
Gender PlayerCatalog::genderOfPart(const QString& stem)
{
    const QString s = stem.toLower();
    // The game-authored markers are tested FIRST: they are explicit, while the
    // Survive prefix rule is positional and would happily claim a name that
    // merely starts with the same two letters.
    if (s.contains(QLatin1String("_plyf"))) return Gender::Female;
    if (s.contains(QLatin1String("_plym"))) return Gender::Male;
    // The Survive positional rule: family, then gender, then either a number
    // (arf0, avm0) or an underscore (avf_hair_a0). "av" is in this list because
    // the avatar heads and hair are named the same way as every other part —
    // and leaving it out is what made the male survivor's face and hair
    // invisible to the catalogue: avm0_type0_def matched nothing, fell through
    // to Gender::Any, and was then dropped by the "== want" test that every
    // slot applies.
    static const QRegularExpression ssdRe{
        QStringLiteral("^(ar|hd|lg|ua|bd|rg|av)([fm])(\\d|_)")};
    const QRegularExpressionMatch m = ssdRe.match(s);
    if (m.hasMatch())
        return m.captured(2) == QLatin1String("f") ? Gender::Female : Gender::Male;
    static const QRegularExpression eyeRe{QStringLiteral("^(hat|gls)_([fm])\\d")};
    const QRegularExpressionMatch e = eyeRe.match(s);
    if (e.hasMatch())
        return e.captured(2) == QLatin1String("f") ? Gender::Female : Gender::Male;
    // A hat's female fit is the same model with a _f tail; the bare name is the
    // male one, but only inside the hat family — "_f" is far too common a tail
    // to read as a gender anywhere else.
    if (s.startsWith(QLatin1String("hat")))
        return s.endsWith(QLatin1String("_f")) ? Gender::Female : Gender::Male;
    return Gender::Any;
}

// The StrCode32 an FMDL stores for a bone is the low 32 bits of the legacy name
// hash. Computed rather than written down as a constant so it can never drift
// from the hash function the archives are indexed with.
quint32 boneCode(const char* name)
{
    return static_cast<quint32>(
        hashFileNameLegacy(QLatin1String(name), /*removeExtension=*/false)
        & 0xFFFFFFFFu);
}

const PlayerCatalog& PlayerCatalog::instance()
{
    static PlayerCatalog c;
    const ArchiveIndex& index = ArchiveIndex::instance();
    const void* key = index.files().constData();
    const int count = index.files().size();
    const int gen = GameFilter::instance().generation();
    if (c.m_indexKey != key || c.m_indexCount != count
        || c.m_filterGeneration != gen) {
        c.m_indexKey = key;
        c.m_indexCount = count;
        c.m_filterGeneration = gen;
        c.build();
    }
    return c;
}

const PlayerSubject* PlayerCatalog::find(const QString& id) const
{
    for (const PlayerSubject& s : m_subjects)
        if (s.id == id) return &s;
    return nullptr;
}

void PlayerCatalog::build()
{
    m_subjects.clear();
    const ArchiveIndex& index = ArchiveIndex::instance();
    const GameFilter& filter = GameFilter::instance();

    // Model stem → file index, once, for each game the filter allows. Shadowed
    // copies lose: they are the ones the game would not load.
    QHash<QString, int> tpp, mgo, gz, ssd;
    // Every av* model in the install, whatever game its path says.
    QHash<QString, int> avatarAny;
    const auto& files = index.files();
    for (int i = 0; i < files.size(); ++i) {
        const IndexedFile& f = files[i];
        if (!f.named) continue;
        if (!f.path.endsWith(QLatin1String(".fmdl"), Qt::CaseInsensitive)) continue;
        const QString stemEarly =
            f.path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
        // Survive's avatar models are named under /Assets/tpp/, so classifying
        // by path files them as TPP — and then turning TPP OFF would take the
        // survivor's own face and hair away with it, which is not what that
        // checkbox means. The avatar set is collected regardless of the filter
        // and handed to buildSurvive separately.
        //
        // BEFORE the filter test, and that is the whole point. Collecting after
        // it looks equivalent and is not: the entry has already been skipped by
        // then, so with only Survive ticked the male survivor's heads and hair
        // (avm0_type*, avm_hair_*, which resolve under tpp) never reached this
        // map, while the female's (avf*, which resolve under ssd) did — the
        // male lost his Face Preset and Hairstyle rows and fell back to the
        // base body's untextured head.
        if (stemEarly.startsWith(QLatin1String("av"), Qt::CaseInsensitive)) {
            // Which ARCHIVE an entry came out of, not which asset path it
            // resolves to. MGO ships its own avatar under the same names —
            // avm0_type0_def, avm_hair_a0_v0_cov — in the same /Assets/tpp/
            // tree, so stem alone cannot tell the two games' avatars apart and
            // an MGO head could take the survivor's slot. The archive knows:
            // its game is a majority vote over everything inside it.
            const auto gameOfArchive = [&](int fileIndex) {
                const int a = files[fileIndex].archiveId;
                return a >= 0 && a < index.archives().size()
                    ? index.archives()[a].game
                    : GameId::Unknown;
            };
            const bool mine = gameOfArchive(i) == GameId::Survive;
            const auto ex = avatarAny.constFind(stemEarly);
            if (ex == avatarAny.constEnd()) {
                avatarAny.insert(stemEarly, i);
            } else {
                const bool theirs = gameOfArchive(ex.value()) == GameId::Survive;
                // Survive's own copy wins outright; between two copies of the
                // same standing the un-shadowed one does, as everywhere else.
                if ((mine && !theirs)
                    || (mine == theirs && !f.shadowed && files[ex.value()].shadowed))
                    avatarAny.insert(stemEarly, i);
            }
        }
        const GameId g = index.gameOf(f);
        if (!filter.enabled(g)) continue;
        QHash<QString, int>* into = nullptr;
        switch (g) {
            case GameId::Tpp: into = &tpp; break;
            case GameId::Mgo: into = &mgo; break;
            case GameId::GroundZeroes: into = &gz; break;
            case GameId::Survive: into = &ssd; break;
            default: break;
        }
        if (!into) continue;
        const QString& stem = stemEarly;
        const auto it = into->constFind(stem);
        if (it == into->constEnd()) { into->insert(stem, i); continue; }
        if (!f.shadowed && files[it.value()].shadowed) into->insert(stem, i);
    }

    if (filter.enabled(GameId::Survive)) buildSurvive(ssd, avatarAny);
    // BOTH maps: MGO 3 installs as its own .dat pair under <install>/mgo/ and
    // carries the female avatar, while The Phantom Pain's own male-only avatar
    // editor lives in the TPP tree. See buildMgo.
    if (filter.enabled(GameId::Mgo)) buildMgo(mgo, tpp);
    if (filter.enabled(GameId::Tpp)) {
        buildTpp(tpp);
        buildTppCharacters(tpp);
    }
    if (filter.enabled(GameId::GroundZeroes)) buildGz(gz);
    // ONE order, decided here. builderSource() used to sort its own copy while
    // m_subjects stayed in whatever order the games were read and the derived
    // pass walked its hash — so the list on screen and the dump the harness
    // gates on disagreed, and the dump disagreed with itself run to run.
    std::stable_sort(m_subjects.begin(), m_subjects.end(),
                     [](const PlayerSubject& a, const PlayerSubject& b) {
                         if (a.derived != b.derived) return !a.derived;
                         if (a.game != b.game) return int(a.game) < int(b.game);
                         return a.name.localeAwareCompare(b.name) < 0;
                     });
    qInfo("players: %s | games: %s", qUtf8Printable(describe()),
          qUtf8Printable(filter.describe()));
}

// ── Metal Gear Survive ──────────────────────────────────────────────────────
void PlayerCatalog::buildSurvive(const QHash<QString, int>& ssdModels,
                                 const QHash<QString, int>& avatarModels)
{
    if (ssdModels.isEmpty()) return;
    // Survive's AVATAR models are not all named under /Assets/ssd/. Depending
    // on which container an entry came out of, avf0_type*_def resolves under
    // ssd while avm0_type*_def resolves under tpp — the qar dictionary names
    // every one of them /Assets/tpp/chara/avm/Scenes/, and only the copies that
    // arrive as FPK members carry an ssd path. Classifying by path then puts
    // the two genders in different maps, and buildSurvive saw only one of them:
    // that is why the male survivor had no Face Preset row and fell back to the
    // base body's untextured head.
    //
    // So the avatar set is looked up in BOTH maps, ssd first. buildMgo already
    // does the same thing for the same reason, and says so.
    QHash<QString, int> models = ssdModels;
    for (auto it = avatarModels.constBegin(); it != avatarModels.constEnd(); ++it)
        if (it.key().startsWith(QLatin1String("av")) && !models.contains(it.key()))
            models.insert(it.key(), it.value());

    // The game's OWN per-gender tables for the two shared families. Survive
    // ships one variation file per item per gender under
    // /Assets/ssd/**/fova/hats and .../fova/glasses:
    //
    //   hats:    hat_f13  hat_f15  hat_f21  hat_f31
    //            hat_m09  hat_m10  hat_m13  hat_m16  hat_m21  hat_m31
    //   glasses: eye_f04  eye_m04                    (the models are gls4)
    //
    // That is authored evidence and it beats anything inferred from model
    // names. It also corrects a guess I had made from the names alone: a hat
    // that ships only a bare model is NOT shared between the survivors — hat9,
    // hat10 and hat16 each have a hat_mNN and no hat_fNN, so they are the
    // male's. The two sources agree on every hat the table covers.
    QSet<QString> fovaFit;   // "hat|f|13", "gls|m|4" — leading zeros dropped
    {
        static const QRegularExpression re{QStringLiteral(
            "/fova/(hats|glasses)/(?:hat|eye)_([fm])(\\d+)\\.")};
        for (const IndexedFile& f : ArchiveIndex::instance().files()) {
            const QRegularExpressionMatch m = re.match(f.path);
            if (!m.hasMatch()) continue;
            fovaFit.insert((m.captured(1) == QLatin1String("hats")
                                ? QStringLiteral("hat|")
                                : QStringLiteral("gls|"))
                           + m.captured(2) + QLatin1Char('|')
                           + QString::number(m.captured(3).toInt()));
        }
    }
    struct SlotRule { const char* prefix; const char* id; const char* label; };
    // Two of these share a slot. Checked by rendering them: every bd* AND
    // every ua* is a COMPLETE torso — bdf0 is the bare torso in underwear,
    // bdf6 the T-shirt, bdf10 an armoured jacket, uaf0 a jacket, uaf1 a plate
    // carrier — so wearing one of each would put two torsos in the same place.
    // The community gear list agrees: the game has one "Torso Equipment" slot.
    // A chest rig (rg*) is webbing worn OVER a torso and stays separate.
    static const SlotRule kRules[] = {
        {"hd", "head_equipment", "Head Equipment"},
        {"ar", "arm",            "Arms"},
        {"bd", "torso",          "Torso"},
        {"ua", "torso",          "Torso"},
        {"lg", "leg",            "Legs"},
        {"rg", "chest_rig",      "Chest Rig"},
    };

    for (int gi = 0; gi < 2; ++gi) {
        const Gender want = gi == 0 ? Gender::Male : Gender::Female;
        const QString baseStem = gi == 0 ? QStringLiteral("bsm0_main0_def")
                                         : QStringLiteral("bsf0_main0_def");
        PlayerSubject s;
        s.id = gi == 0 ? QStringLiteral("ssd_m") : QStringLiteral("ssd_f");
        s.name = gi == 0 ? QStringLiteral("Survivor — Male")
                         : QStringLiteral("Survivor — Female");
        s.game = GameId::Survive;
        s.gender = want;
        s.baseStem = baseStem;
        s.baseFileIdx = models.value(baseStem, -1);
        s.baseIsSkeletonOnly = true;
        s.variantSlot.clear();   // the base body is the character
        s.note = QStringLiteral(
            "Slots and parts from /Assets/ssd/pack/player. Every part name "
            "carries its gender in the third letter — the avatar head and hair "
            "included — so only this survivor's own parts are offered.");

        for (const SlotRule& r : kRules) {
            QVector<CatalogPart> parts;
            for (auto it = models.constBegin(); it != models.constEnd(); ++it) {
                if (!it.key().startsWith(QLatin1String(r.prefix))) continue;
                if (genderOfPart(it.key()) != want) continue;
                parts.append(makePart(QLatin1String(r.id), it.key(), it.value()));
            }
            addSlot(s, QLatin1String(r.id), QLatin1String(r.label), parts);
        }
        // Hats and glasses: same models, a per-gender fit — but the fit is
        // decided by the FAMILY, not by one name. Measured over the shipped
        // hats: hat13/hat21/hat31 ship as a bare name AND a _f, hat15 ships
        // only as _f, and hat9/hat10/hat16/hat30 ship only bare. So a bare name
        // is the male fit only when an _f sibling exists to be the female one;
        // with no sibling it is the single model both survivors wear. Reading
        // "_f means female, bare means male" per name cost the female survivor
        // four of her eleven hats.
        //
        // The glasses prefix is "gls", not "eye" — there is no eye* model in
        // the shipped data at all, which is why the Glasses slot was empty.
        for (const char* pair : {"hat", "gls"}) {
            const bool isHat = pair[0] == 'h';
            QVector<CatalogPart> parts;
            const QString fam = QString::fromLatin1(pair) + QLatin1Char('|');
            static const QRegularExpression numRe{
                QStringLiteral("^(?:hat|gls)(\\d+)")};
            for (auto it = models.constBegin(); it != models.constEnd(); ++it) {
                const QString& k = it.key();
                if (!k.startsWith(QLatin1String(pair))) continue;
                const bool female = k.endsWith(QLatin1String("_f"));
                const QChar wantLetter =
                    want == Gender::Female ? QLatin1Char('f') : QLatin1Char('m');
                // 1. The game's own table, when it covers this item.
                const QRegularExpressionMatch hn = numRe.match(k);
                bool decided = false;
                if (hn.hasMatch()) {
                    const QString num =
                        QString::number(hn.captured(1).toInt());
                    if (fovaFit.contains(fam + QStringLiteral("f|") + num)
                        || fovaFit.contains(fam + QStringLiteral("m|") + num)) {
                        decided = true;
                        if (!fovaFit.contains(fam + wantLetter
                                              + QLatin1Char('|') + num))
                            continue;
                        // Listed for this gender: take the matching fit when
                        // the model ships one, the shared model otherwise.
                        if (female != (want == Gender::Female)
                            && models.contains(want == Gender::Female
                                                   ? k + QLatin1String("_f")
                                                   : k.left(k.size() - 2)))
                            continue;   // the other gender's copy of a pair
                    }
                }
                // 2. Not in the table — fall back to the shape of the names.
                if (!decided) {
                    const bool hasSibling =
                        female ? models.contains(k.left(k.size() - 2))
                               : models.contains(k + QLatin1String("_f"));
                    if (hasSibling) {
                        // A pair: each side is that gender's fit.
                        const Gender g = female ? Gender::Female : Gender::Male;
                        if (g != want) continue;
                    } else if (female && want != Gender::Female) {
                        // Only a _f exists: female-only.
                        continue;
                    }
                }
                parts.append(makePart(
                    QLatin1String(isHat ? "accessory" : "glasses"), k, it.value()));
            }
            addSlot(s, QLatin1String(isHat ? "accessory" : "glasses"),
                    QLatin1String(isHat ? "Accessory" : "Glasses"), parts);
        }
        // The "face" preset is the whole HEAD — avf0_type0_def is a complete
        // head with 290 bones of facial rig, not a face laid over one — so the
        // slot is called what it is. Both survivors are read the same way:
        // av<gender>N_typeN_def for the head, av<gender>_hair_* for the hair.
        // Nothing here is female-only; if a survivor ends up with no head it is
        // because that install ships none, and the log below says so with the
        // evidence rather than leaving an empty combo to be guessed at.
        QVector<CatalogPart> faces, hair;
        QStringList avSeen;
        for (auto it = models.constBegin(); it != models.constEnd(); ++it) {
            const QString& k = it.key();
            if (!k.startsWith(QLatin1String("av"))) continue;   // avatar set only
            avSeen.append(k);
            // The "av" guard is on BOTH branches. Without it on the hair one,
            // any part whose name merely contains "_hair" — a hat with a hair
            // fringe modelled in, say — landed in the Hair slot as well as its
            // own, offering the same model twice on one character.
            // NOT avm_hone — rendering one on its own shows a HORN, which is
            // what MGO already calls it. It is not facial hair, whatever the
            // name suggests.
            if (k.contains(QLatin1String("_hair")))
                { if (genderOfPart(k) == want) hair.append(makePart(QStringLiteral("hair"), k, it.value())); }
            else if (k.contains(QLatin1String("_type"))
                     && genderOfPart(k) == want)
                faces.append(makePart(QStringLiteral("head"), k, it.value()));
        }
        if (faces.isEmpty()) {
            std::sort(avSeen.begin(), avSeen.end());
            qInfo("players: %s has no head model — the %lld avatar model(s) in "
                  "this install are: %s", qUtf8Printable(s.name),
                  qint64(avSeen.size()),
                  qUtf8Printable(avSeen.isEmpty() ? QStringLiteral("(none)")
                                              : avSeen.join(QLatin1Char(' '))));
        }
        // The AVATAR screen does not offer the eight head models — it offers
        // 28 numbered "Basic Face Shape" presets, and the game's own table says
        // which head each one uses. When that table is in the install, the slot
        // IS the game's grid; without it we fall back to the raw models rather
        // than showing nothing.
        // Both survivors get their own table: Survive ships avatar_presets for
        // the men and avatar_presets_women for the women, and the men's grid is
        // as much "the AVATAR screen" as the women's. Gating this on Female was
        // the reason the male page had no Face Preset row and fell back to the
        // base body's untextured head.
        const AvatarPresets& ap = AvatarPresets::instance();
        const AvatarPresets::Sex sex = want == Gender::Male
            ? AvatarPresets::Sex::Men
            : AvatarPresets::Sex::Women;
        if (ap.ok(sex) && !faces.isEmpty()) {
            QHash<QString, int> byStem;
            for (const CatalogPart& f : faces) byStem.insert(f.id, f.modelFileIdx);
            QVector<CatalogPart> presetFaces;
            for (const AvatarPreset& pr : ap.presets(sex)) {
                const QString stem = AvatarPresets::headStemFor(pr.faceType, sex);
                const int idx = byStem.value(stem, -1);
                if (idx < 0) continue;   // that head is not in this install
                CatalogPart cp;
                cp.slot = QStringLiteral("head");
                cp.id = stem;
                cp.displayName = QStringLiteral("Face %1").arg(pr.index + 1);
                cp.modelFileIdx = idx;
                cp.presetIndex = pr.index;
                presetFaces.append(cp);
            }
            if (!presetFaces.isEmpty()) faces = presetFaces;
        }
        // The AVATAR screen's own names: the numbered grid is "Basic Face
        // Shape" and the four hair models are hairstyles.
        addSlot(s, QStringLiteral("head"), QStringLiteral("Face Preset"), faces);
        addSlot(s, QStringLiteral("hair"), QStringLiteral("Hairstyle"), hair);
        // Where a part hangs when its own root bone is not in the wearer.
        // Measured, not assumed: of the 352 shipped Survive character models,
        // 331 root at SKL_000_WAIST, 6 at SKL_002_CHEST, 4 at SKL_004_HEAD, 2
        // at SKL_400_HEADROOT — every one of which the survivor's skeleton
        // carries, so they align themselves — and 4 at SKL_000_ROOT, which the
        // survivor's skeleton does NOT contain at all. Those four (hat15,
        // hat16, hat30 and the tank hose) have nothing to align to and land at
        // the origin, i.e. at the waist. Their vertex bounds are head-sized and
        // centred on nothing, so the slot supplies the anchor.
        for (PlayerSlot& sl : s.slotList)
            if (sl.id == QLatin1String("accessory")
                || sl.id == QLatin1String("hats")
                || sl.id == QLatin1String("head_equipment")
                || sl.id == QLatin1String("glasses")
                || sl.id == QLatin1String("hair"))
                sl.anchor = boneCode("SKL_004_HEAD");
        // What a new survivor starts in. The naked base body is the canvas,
        // not the default outfit: the game puts them in a T-shirt, bare arms
        // and plain trousers, and gives the female avatar a head preset (the
        // male has none in the shipped data and keeps the base body's head).
        // Built by concatenation, NOT QString::arg: "bd%16_main0_def" makes Qt
        // see placeholder %16, not %1 followed by a 6, and quietly yields
        // "bdf_main0_def".
        const QString g = gi == 0 ? QStringLiteral("m") : QStringLiteral("f");
        const auto defaultTo = [&](const char* slot, const QString& stem) {
            if (models.contains(stem)) s.defaults.insert(QLatin1String(slot), stem);
        };
        defaultTo("torso", QStringLiteral("bd") + g + QStringLiteral("0_main0_def"));
        defaultTo("arm",   QStringLiteral("ar") + g + QStringLiteral("0_main0_def"));
        defaultTo("leg",   QStringLiteral("lg") + g + QStringLiteral("0_main0_def"));
        // The starting face is this survivor's OWN first head, whatever it is
        // named — not a hard-coded avf0. A hard-coded default is a default only
        // one of the two can ever have.
        for (const PlayerSlot& sl : s.slotList)
            if (sl.id == QLatin1String("head") && !sl.parts.isEmpty()) {
                s.defaults.insert(QStringLiteral("head"), sl.parts.first().id);
                break;
            }
        if (!s.slotList.isEmpty() || s.baseFileIdx >= 0) m_subjects.append(s);
    }
}

// ── Metal Gear Online 3 ─────────────────────────────────────────────────────
//
// Two archive sets can carry this avatar and BOTH are read.
//
//   /Assets/tpp/…   the avatar editor that ships inside The Phantom Pain. It is
//                   male only: the game's own pack is plparts_avatar_man with
//                   no woman counterpart, there is one body (avm0_body0_def),
//                   and the head and hair families are avm exclusively.
//   /Assets/mgo/…   MGO 3 proper, which installs as its OWN pair of .dat files
//                   under <install>/mgo/ rather than alongside the TPP chunks.
//                   That set carries the female avatar — avf0_type0…7, its own
//                   avf_hair styles and its own deform table.
//
// Before this the avatar was built from the TPP map alone, so pointing the
// browser at an install's mgo folder changed nothing and the female avatar wore
// the male's face and hair. Now MGO's own copy of a stem wins and TPP's is the
// fallback, which is also the order the game loads them in.
//
// THERE IS NO WARDROBE COMBO on this page. The avatar is dressed entirely by
// the game's four gear categories; the model behind them is the skeleton
// carrier skl0_main0_def{_f} and it is never drawn. What used to be the
// "Outfit" row — the bare avatar body plus every DLC suit that ships an
// <id>_plym0_def / _plyf0_def fit — was a second torso underneath the Base
// garment. Those suits are Snake's, and buildTpp lists them on his page.
//
// The avatar's own body packs (/pack/player/avatar/body/plfova_<id>_main0_
// skin0_c<NN>) are still read, for the one thing they alone say: how many skin
// tones the install ships.
void PlayerCatalog::buildMgo(const QHash<QString, int>& mgoModels,
                             const QHash<QString, int>& tppModels)
{
    if (mgoModels.isEmpty() && tppModels.isEmpty()) return;

    // MGO's own copy of a stem wins; TPP's is the fallback.
    QHash<QString, int> models = tppModels;
    for (auto it = mgoModels.constBegin(); it != mgoModels.constEnd(); ++it)
        models.insert(it.key(), it.value());

    // ── How many skin tones this install ships ───────────────────────────────
    // /Assets/<game>/pack/player/avatar/body/plfova_<id>_main0_skin0_c<NN>
    QSet<int> tones;
    {
        static const QRegularExpression bodyRe{QStringLiteral(
            "/pack/player/avatar/body/plfova_[a-z0-9]+_main0_skin0_c(\\d+)")};
        const ArchiveIndex& index = ArchiveIndex::instance();
        const auto& files = index.files();
        for (const IndexedFile& f : files) {
            if (!f.named) continue;
            const QRegularExpressionMatch m = bodyRe.match(f.path);
            if (!m.hasMatch()) continue;
            tones.insert(m.captured(1).toInt());
        }
    }

    for (int gi = 0; gi < 2; ++gi) {
        const Gender want = gi == 0 ? Gender::Male : Gender::Female;
        const QString gTag = gi == 0 ? QStringLiteral("avm") : QStringLiteral("avf");
        PlayerSubject s;
        s.id = gi == 0 ? QStringLiteral("mgo_m") : QStringLiteral("mgo_f");
        s.name = gi == 0 ? QStringLiteral("MGO Avatar — Male")
                         : QStringLiteral("MGO Avatar — Female");
        s.game = GameId::Mgo;
        s.gender = want;
        // THE MGO AVATAR HAS NO OUTFIT COMBO. What this page used to call the
        // "Body" — the bare avatar body plus every DLC suit that ships an
        // avatar fit — was never the thing MGO dresses. The game poses ONE
        // model per gender, /Assets/mgo/chara/base/Scenes/skl0_main0_def{_f}
        // (measured: it ships inside the mgo chunk, in the FPK
        // /Assets/mgo/pack/collectible/common/col_common_mgo, with its .fcnp
        // and .frdv), and that model is a skeleton carrier which is never
        // drawn — everything the player sees is a fova part stacked on it,
        // exactly as Survive stacks parts on bsm0/bsf0. Drawing the bare body
        // under a Base garment put two torsos in the same place.
        //
        // So variantSlot stays EMPTY: builderSource() then puts the base model
        // itself into the Body combo as its single entry, and fillVariantList()
        // disables a one-entry combo — the Survivor's behaviour, a greyed combo
        // that still names the model. The DLC suits are Snake's own fits and
        // are listed on HIS page; see buildTpp.
        //
        // baseIsSkeletonOnly is decided AFTER the gear section, not here: it is
        // only safe to hide the base on a page that has gear to put on instead.
        // See the "dressable" test below.

        // Does this install carry THIS gender's own head and hair? MGO 3's
        // archives do; The Phantom Pain's alone do not.
        // The predicate has to be the SAME one the consumer below uses, or a
        // gender can be told it has its own set and then match nothing — an
        // "avf0_type0_hair0" would have flipped ownHair on while the hair loop
        // looks for "avf_hair…", leaving the row empty AND having discarded the
        // shared fallback it would otherwise have had. So each test is passed
        // the exact prefix its consumer will use.
        //
        // …and it has to exclude SURVIVE's avatar — but ONLY Survive's. An
        // earlier pass concluded "MGO ships no female head at all" from a
        // container copy of mgo_chunk0.dat that was 0.57% allocated. That is
        // FALSE, and it is the sparse-archive trap in its purest form:
        // measured against the real install's mgo chunk (pulled straight from
        // /Assets/mgo/pack/player/avatar/*), MGO ships avf0_type0..7 and
        // avf_hair_{a0,b0,c0,d0}_v0_cov under /Assets/mgo/chara/avm/Scenes/ —
        // its own copies of the avm heads and hair too. The woman's page must
        // list HER heads, not fall back to the male set.
        //
        // What stays true is the cross-game guard: Survive ships DIFFERENT
        // models under these same stems, addressed by its own look pipeline,
        // and they must never stand in here. Judged by the ARCHIVE the entry
        // came out of, not by its asset path — Survive's avatar copies that
        // arrive through the qar dictionary resolve under /Assets/tpp/, so a
        // path test alone lets exactly the models this guard exists to keep
        // out walk straight past it. The path test is kept for loose-mounted
        // ssd trees, whose archive vote is not Survive.
        const auto fromSurvive = [](int fileIdx) {
            const ArchiveIndex& ix = ArchiveIndex::instance();
            const auto& files = ix.files();
            if (fileIdx < 0 || fileIdx >= files.size()) return false;
            const int a = files[fileIdx].archiveId;
            if (a >= 0 && a < ix.archives().size()
                && ix.archives()[a].game == GameId::Survive)
                return true;
            return files[fileIdx].path.startsWith(QLatin1String("/Assets/ssd/"),
                                                  Qt::CaseInsensitive);
        };
        const auto anyStartingWith = [&models, &fromSurvive](
                                         const QString& prefix,
                                         const QString& infix) {
            for (auto it = models.constBegin(); it != models.constEnd(); ++it)
                if (it.key().startsWith(prefix)
                    && (infix.isEmpty() || it.key().contains(infix))
                    && !fromSurvive(it.value()))
                    return true;
            return false;
        };
        const bool ownFaces = anyStartingWith(gTag, QStringLiteral("_type"));
        const bool ownHair =
            anyStartingWith(gTag + QStringLiteral("_hair"), QString());

        // ── The animation base, and its stand-in ────────────────────────
        // The one model the game poses. Its own path, measured out of the
        // install's mgo chunk:
        //   /Assets/mgo/chara/base/Scenes/skl0_main0_def.fmdl     (male)
        //   /Assets/mgo/chara/base/Scenes/skl0_main0_def_f.fmdl   (female)
        // Where the page can be dressed it is never drawn, so what it resolves
        // to matters only for the bones the fova parts hang from and for the
        // name the greyed Body combo shows.
        //
        // The bare avatar body is the stand-in for an install that does not
        // carry the base pack: avm0/avf0_body0_def carries the same skeleton.
        // This gender's own first, then the male's, and only ever the male's —
        // The Phantom Pain ships a single avatar body under the avm name and
        // that is what the female falls back to, exactly as she falls back to
        // the shared face and hair. The reverse is not a fallback, it is a
        // mistake.
        //
        // fromSurvive on every candidate, like the face and hair loops below
        // and the gear join above: `models` is tpp ∪ mgo and Survive's avatar
        // copies resolve under /Assets/tpp/ paths, so a name test alone would
        // let another game's body in as this avatar's skeleton.
        const auto pickModel = [&models, &fromSurvive](const QString& stem) {
            const auto it = models.constFind(stem);
            return (it != models.constEnd() && !fromSurvive(it.value()))
                ? it.value() : -1;
        };
        QString sklStem = QStringLiteral("skl0_main0_def")
            + (want == Gender::Female ? QStringLiteral("_f") : QString());
        int sklIdx = pickModel(sklStem);
        if (sklIdx < 0) sklStem.clear();
        // The bare avatar body, kept separately: which of the two becomes the
        // base depends on whether this page has anything to dress the avatar
        // IN, and that is not known until the gear section below has run.
        QString bodyStem;
        int bodyIdx = -1;
        for (const QString& cand : {gTag + QStringLiteral("0_body0_def"),
                                    QStringLiteral("avm0_body0_def")}) {
            const int i = pickModel(cand);
            if (i >= 0) { bodyStem = cand; bodyIdx = i; break; }
        }

        QVector<CatalogPart> faces, hair, horn;

        for (auto it = models.constBegin(); it != models.constEnd(); ++it) {
            const QString& k = it.key();
            // ── Face and hair ────────────────────────────────────────────────
            // This gender's own set when the install has one; the male set as
            // a shared fallback when it does not, because The Phantom Pain's
            // avatar editor genuinely has only the one.
            const QString facePrefix = ownFaces ? gTag : QStringLiteral("avm");
            const QString hairPrefix = ownHair ? gTag : QStringLiteral("avm");
            // The face slot is called "head", the same as every other avatar's.
            // That is not cosmetic: it is the id the appearance rows look for,
            // and calling it "face" here was half the reason the MGO avatar had
            // no Eye Colour, Skin Colour or Beard rows while the Survivor did.
            // The same Survive exclusion the predicate above uses, applied at
            // the consumer as well — the two have to agree or a gender is told
            // it has its own set and then matches nothing.
            if (fromSurvive(it.value())) continue;
            if (k.startsWith(facePrefix) && k.contains(QLatin1String("_type")))
                faces.append(makePart(QStringLiteral("head"), k, it.value()));
            else if (k.startsWith(hairPrefix + QStringLiteral("_hair")))
                hair.append(makePart(QStringLiteral("hair"), k, it.value()));
            else if (k.startsWith(QLatin1String("avm_hone")))
                horn.append(makePart(QStringLiteral("horn"), k, it.value()));
        }

        // The game's own face grid, which is the OTHER half. The avatar editor
        // does not offer sixteen head models — it offers a numbered grid of
        // presets, each naming a head plus a hairstyle, brow, beard, skin tone
        // and two eye colours, and /Assets/tpp/ui/Script/avatar_presets.lua is
        // that grid. Twenty-eight rows for the men. The women's table is
        // avatar_presets_women; where an install has neither, the raw head
        // models stand in rather than showing nothing.
        //
        // Applying it is what makes the appearance rows appear at all: they are
        // gated on the head slot's parts carrying a preset index, because that
        // is what separates an avatar's face from a soldier's helmet slot.
        const AvatarPresets& ap = AvatarPresets::instance();
        const AvatarPresets::Sex sex = want == Gender::Male
            ? AvatarPresets::Sex::Men
            : AvatarPresets::Sex::Women;
        bool gridApplied = false;
        if (ap.ok(sex) && !faces.isEmpty()) {
            QHash<QString, int> byStem;
            for (const CatalogPart& f : faces) byStem.insert(f.id, f.modelFileIdx);
            QVector<CatalogPart> presetFaces;
            QSet<QString> used;
            for (const AvatarPreset& pr : ap.presets(sex)) {
                const QString stem = AvatarPresets::headStemFor(pr.faceType, sex);
                const int idx = byStem.value(stem, -1);
                if (idx < 0) continue;   // that head is not in this install
                CatalogPart cp;
                cp.slot = QStringLiteral("head");
                cp.id = stem;
                cp.displayName = QStringLiteral("Face %1").arg(pr.index + 1);
                cp.modelFileIdx = idx;
                cp.presetIndex = pr.index;
                presetFaces.append(cp);
                used.insert(stem);
            }
            if (!presetFaces.isEmpty()) {
                // Heads the grid never names stay on the list, after it. The
                // preset table only ever points at the avm0 family, so
                // replacing the list wholesale traded sixteen head models for
                // twenty-eight presets over eight of them and put the whole
                // avm1 family out of reach. They carry no preset index, so the
                // slot's own sort puts them ahead of the numbered grid and
                // nothing downstream mistakes them for presets.
                for (const CatalogPart& f : faces)
                    if (!used.contains(f.id)) presetFaces.append(f);
                faces = presetFaces;
                gridApplied = true;
            }
        }

        // ── MGO3's four gear categories, FROM THE GAME'S OWN TABLE ──────
        //
        // /Assets/mgo/level_asset/config/GearConfig.lua lists every item the
        // avatar can wear, split by gender, in four categories — Headgear,
        // Chest, Base and Accessory (its key for that one is "Eyes"), each
        // item with its own name tag, icon and colour palette.
        //
        // The section is keyed by CLASS first — Infil, Recon, Tech — so each
        // of those categories occurs three times per gender with heavy
        // overlap. MgoGearConfig merges and de-duplicates them; what reaches
        // here per gender is 42 headgear, 25 (M) / 26 (F) chest, 14 base and
        // 6 accessories: 87 male / 88 female.
        //
        // Built from the table rather than from model discovery, and that is
        // the whole point: an earlier pass matched on the directory a model
        // sat in, which meant a gender could only be told apart by the model's
        // name — and MGO names the female fit with a trailing _f and leaves
        // the male fit bare, so "no _f" reads as both "male" and "shared".
        // That is why the man's page was offering the woman's gear. The
        // config's Male and Female lists have no such ambiguity.
        //
        // An item whose model this install does not carry is dropped rather
        // than listed dead, and the count of those is logged, so a partial
        // install shows what it has instead of a list of things that will not
        // load.
        {
            const MgoGearConfig& gc = MgoGearConfig::instance();
            const bool female = want == Gender::Female;
            // ── THE ID→MODEL JOIN IS THE GAME'S OWN, NOT A RULE ─────────
            // MGO ships one fova pack PER GEAR ITEM ID:
            //   /Assets/mgo/pack/player/fova/<id>.fpk
            // whose members include /Assets/mgo/fova/chara/<dir>/<id>.fv2 —
            // dir "head"/"hat" for Headgear, "chest", "body" for Base, "eyes"
            // for Accessory — and that .fv2's external-file table names the
            // exact model the item wears. Measured over every one of the 175
            // item ids on the real install, and it REFUTES both earlier
            // readings of the id scheme:
            //   * the number is not a model index (inb_f00 → icl1, not icl0),
            //   * and it is not an ordinal rank either — the Base families
            //     map DESCENDING (inb_f00→icl1, inb_f03→icl0; teb_m02→tcl1,
            //     teb_m03→tcl0), which the game's own Defaults confirm:
            //     Base defaults are ?eb_?03, and the Enforcer default
            //     teb_?03 is the BDU, tcl0_main0_def{_f}.
            // The eyewear fv2 lists only the model's .frdv, which sits in the
            // same Scenes/ directory as the .fmdl and therefore shares its
            // path hash — the extension-blind lookup below resolves both.
            //
            // So: resolve through the fv2 when the install carries it, and
            // fall back to MgoGearItem::modelStems() candidate guessing only
            // where it does not. The fv2 is matched under /Assets/mgo/ only —
            // Survive ships its own fova tree under /Assets/ssd/ with
            // colliding ids (hat21, gls4), the same cross-game trap as ever.
            QHash<QString, int> fovaById;        // "cms_f01" → .fv2 file idx
            QHash<quint64, int> modelByPathHash; // path-hash-minus-ext → .fmdl
            {
                // The same contract as the `models` map this section used to
                // resolve through: only games the filter allows, and never a
                // file out of a SURVIVE archive — fromSurvive's archive vote,
                // not a path test, because Survive's copies of reused models
                // resolve under /Assets/tpp/ (the same trap the face section
                // above documents). Without these two tests the hash map
                // quietly widened the join to every game in the index.
                const ArchiveIndex& ix = ArchiveIndex::instance();
                const GameFilter& filter = GameFilter::instance();
                const auto& fl = ix.files();
                for (int i = 0; i < fl.size(); ++i) {
                    const IndexedFile& f = fl[i];
                    if (!f.named) continue;
                    if (fromSurvive(i)) continue;
                    if (f.path.endsWith(QLatin1String(".fv2"),
                                        Qt::CaseInsensitive)
                        && f.path.startsWith(
                            QLatin1String("/Assets/mgo/fova/chara/"),
                            Qt::CaseInsensitive)) {
                        const QString id = f.path
                                               .section(QLatin1Char('/'), -1)
                                               .section(QLatin1Char('.'), 0, 0);
                        const auto ex = fovaById.constFind(id);
                        // Un-shadowed copy wins, as everywhere else.
                        if (ex == fovaById.constEnd()
                            || (!f.shadowed && fl[ex.value()].shadowed))
                            fovaById.insert(id, i);
                    } else if (f.path.endsWith(QLatin1String(".fmdl"),
                                               Qt::CaseInsensitive)) {
                        const GameId g = ix.gameOf(f);
                        if ((g != GameId::Tpp && g != GameId::Mgo)
                            || !filter.enabled(g))
                            continue;
                        const quint64 key = f.hash & kPathMask;
                        const auto ex = modelByPathHash.constFind(key);
                        if (ex == modelByPathHash.constEnd()
                            || (!f.shadowed && fl[ex.value()].shadowed))
                            modelByPathHash.insert(key, i);
                    }
                }
            }
            // A gear model has to be a CHARACTER model from MGO or The
            // Phantom Pain. Both halves earn their place, measured:
            //   /Assets/tpp/item/gls/Scenes/gls0_main0_def.fmdl  — TPP's
            //     glasses ITEM, which the "gls" candidate for eye_m00 matched
            //     and which is not MGO eyewear at all;
            //   /Assets/ssd/chara/glasses/Scenes/gls4_main0_def.fmdl —
            //     SURVIVE's, the same cross-game mix-up that put Survive's
            //     heads on the MGO woman's face list.
            // So: under a /chara/ tree, and not under /Assets/ssd/. MGO does
            // legitimately reuse TPP character models, which is why this is
            // not simply "must be /Assets/mgo/".
            const auto gearPathOk = [](int fileIdx) {
                const auto& fl = ArchiveIndex::instance().files();
                if (fileIdx < 0 || fileIdx >= fl.size()) return false;
                const QString& p = fl[fileIdx].path;
                return p.contains(QLatin1String("/chara/"), Qt::CaseInsensitive)
                    && !p.startsWith(QLatin1String("/Assets/ssd/"),
                                     Qt::CaseInsensitive);
            };
            int missing = 0, collided = 0, byFova = 0, byGuess = 0, merged = 0;
            QStringList unresolved, slotCounts;
            QSet<QString> claimed;
            // The chest half of a two-piece garment, keyed by the Base item it
            // names in BaseGearID. Collected during the pass and applied after
            // it, because the Chest category is listed BEFORE Base and a rule
            // that depended on that order would break the day the config was
            // reordered.
            struct Companion { QString gearId; int fileIdx; };
            QHash<QString, Companion> companions;
            for (const MgoGearCategory& cat : gc.categories(female)) {
                QVector<CatalogPart> parts;
                QSet<QString> seen;
                int mergedInThisSlot = 0;
                for (const MgoGearItem& item : cat.items) {
                    // FIRST the game's own join: the item's .fv2, whose
                    // external-file table names the model (or its .frdv,
                    // which shares the model's path hash). Only THEN the
                    // modelStems() candidate walk, for installs that do not
                    // carry the fova packs.
                    QString stem;
                    int idx = -1;
                    const int fvIdx = fovaById.value(item.id, -1);
                    if (fvIdx >= 0) {
                        const ArchiveIndex& ix = ArchiveIndex::instance();
                        FovaFile fv;
                        if (fv.parse(ix.readFile(ix.files()[fvIdx]))) {
                            for (const quint64 code : fv.files()) {
                                const int i = modelByPathHash.value(
                                    code & kPathMask, -1);
                                if (i >= 0 && gearPathOk(i)) {
                                    idx = i;
                                    stem = ix.files()[i]
                                               .path
                                               .section(QLatin1Char('/'), -1)
                                               .section(QLatin1Char('.'), 0, 0);
                                    break;
                                }
                            }
                        }
                        if (idx >= 0) ++byFova;
                    }
                    if (idx < 0) {
                        for (const QString& cand : item.modelStems(female)) {
                            const int i = models.value(cand, -1);
                            if (i >= 0 && gearPathOk(i)) {
                                stem = cand;
                                idx = i;
                                break;
                            }
                        }
                        if (idx >= 0) ++byGuess;
                    }
                    if (idx < 0) {
                        // Nothing this item might name is here. Report it by
                        // ID — the stem is a guess and naming a guessed file
                        // as "missing" is what makes a log unreadable.
                        ++missing;
                        unresolved << item.id;
                        continue;
                    }
                    // Only a RESOLVED stem is de-duplicated. The old order
                    // tested `seen` first, so when two items resolved to one
                    // model the second was dropped before the missing/
                    // unresolved bookkeeping ran — silently, and precisely in
                    // the ambiguous cases the candidate list exists to expose.
                    // Two items sharing a model is itself worth knowing, so it
                    // is counted rather than swallowed.
                    if (seen.contains(stem)) { ++collided; continue; }
                    seen.insert(stem);
                    claimed.insert(stem);
                    // A chest row bound to a Base row is HALF a garment. It is
                    // held back here and folded into its Base row below, so it
                    // never appears in the Chest list on its own — which is
                    // what the game does and what a player expects: one entry
                    // named once, wearing both models.
                    if (!item.baseGearId.isEmpty()) {
                        companions.insert(item.baseGearId, {item.id, idx});
                        ++merged;
                        ++mergedInThisSlot;
                        continue;
                    }
                    CatalogPart cp = makePart(cat.slotId, stem, idx);
                    // The in-game name when the language catalogue carries it,
                    // the item id otherwise — never the model stem, which is
                    // what the player would least recognise.
                    const QString named =
                        NameCatalog::instance().textForLabel(item.nameTag);
                    cp.displayName = named.isEmpty() ? item.id : named;
                    // Carried for the exclusion pass in the Customize tab —
                    // the game's own "cannot be worn together" rule.
                    cp.gearId = item.id;
                    cp.gearExclude = item.exclude;
                    cp.gearMust = item.must;
                    cp.gearIcon = item.swatch;
                    parts.append(cp);
                }
                addSlot(s, cat.slotId, cat.label, parts);
                // The fraction counts a folded chest half as resolved: it IS
                // in the scene, on its Base row. Reporting 24/25 here would
                // read as a missing item and send the next person looking for
                // a model that is not missing at all.
                slotCounts << QStringLiteral("%1 %2/%3")
                                  .arg(cat.label)
                                  .arg(parts.size() + mergedInThisSlot)
                                  .arg(cat.items.size());
            }
            // Fold each held-back chest half into the Base row it names. Done
            // after every category is built, so it does not depend on Chest
            // being listed before Base.
            int folded = 0;
            if (!companions.isEmpty())
                for (PlayerSlot& sl : s.slotList)
                    for (CatalogPart& p : sl.parts) {
                        const auto it = companions.constFind(p.gearId);
                        if (it == companions.constEnd()) continue;
                        p.companionGearId = it.value().gearId;
                        p.companionFileIdx = it.value().fileIdx;
                        ++folded;
                    }
            if (merged > 0)
                qInfo("players: %s — %d two-piece garment(s): %d chest half"
                      "(s) folded into the Base item that names them in "
                      "BaseGearID%s",
                      qUtf8Printable(s.name), merged, folded,
                      folded == merged
                          ? ""
                          : " — SOME FOUND NO BASE ROW");
            // THE COMPLETENESS CHECK, logged every build: per slot, how many
            // of the config's items resolved to a listed model, and through
            // which join. On a full install every fraction must read N/N with
            // everything "via the game's own fova packs" — the counts matching
            // exactly on both sides is the proof the join is complete, so a
            // shortfall here is a defect, not noise.
            qInfo("players: %s — gear: %s (%d via the game's own fova packs, "
                  "%d via name guessing, %d unresolved)",
                  qUtf8Printable(s.name),
                  qUtf8Printable(slotCounts.join(QLatin1String(", "))), byFova,
                  byGuess, missing);
            if (collided > 0)
                qInfo("players: %s — %d gear item(s) resolved to a model "
                      "another item had already claimed and were listed once",
                      qUtf8Printable(s.name), collided);
            if (missing > 0) {
                qInfo("players: %s — %d gear item(s) in GearConfig.lua have no "
                      "model in this install and were left out",
                      qUtf8Printable(s.name), missing);
                // THE DIAGNOSTIC THAT CLOSES THE LOOP. Two of the four
                // categories have an id->model rule that could not be checked
                // where this was written, so rather than leave a guess in the
                // code with no way to test it, an install that HAS the models
                // prints both sides of the join: the item ids nothing
                // resolved, and the character models no item claimed.
                // Reading those two lists against each other gives the real
                // mapping outright.
                unresolved.sort();
                const QSet<QString> unresolvedIds(unresolved.constBegin(),
                                                  unresolved.constEnd());
                qInfo("players: %s — unresolved gear id(s): %s",
                      qUtf8Printable(s.name),
                      qUtf8Printable(unresolved.join(QLatin1Char(' '))));
                // The OTHER side of the join. Two things decide what belongs
                // in it, and getting either wrong makes the listing useless:
                //
                // SCOPE. It has to be collected under the SAME rule
                // gearPathOk accepts. Filtering this half on "/mgo/chara/"
                // while the resolver accepts any /chara/ path outside Survive
                // left every reused Phantom Pain character model out — and
                // those are exactly the models the unresolved eyewear and BDU
                // ids have to be matched against.
                //
                // …AND RELEVANCE, which the scope fix on its own destroyed.
                // Every non-Survive character model is on the order of a few
                // thousand stems; printed alphabetically and cut at a few
                // hundred, the listing stops around the letter c and neither
                // "gls" nor "tcl" — the two families the whole diagnostic
                // exists to pin down — ever appears. Truncating a sorted list
                // is the one ordering that guarantees the answer is in the
                // discarded tail.
                //
                // So the listing is narrowed to the model FAMILIES the
                // unresolved items actually guessed at: the leading letters of
                // every candidate stem those items offered. That is a handful
                // of three-letter families, it fits whole, and it puts the
                // guesses and the real files side by side.
                QSet<QString> families;
                for (const MgoGearCategory& cat : gc.categories(female))
                    for (const MgoGearItem& item : cat.items) {
                        if (!unresolvedIds.contains(item.id)) continue;
                        for (const QString& cand : item.modelStems(female))
                            families.insert(cand.left(3));
                    }
                QStringList unclaimed;
                for (auto it = models.constBegin(); it != models.constEnd();
                     ++it) {
                    if (claimed.contains(it.key())) continue;
                    if (!families.contains(it.key().left(3))) continue;
                    if (gearPathOk(it.value())) unclaimed << it.key();
                }
                unclaimed.sort();
                QStringList fam(families.constBegin(), families.constEnd());
                fam.sort();
                qInfo("players: %s — %d model(s) in the guessed familie(s) "
                      "[%s] that no gear item claimed: %s",
                      qUtf8Printable(s.name), int(unclaimed.size()),
                      qUtf8Printable(fam.join(QLatin1Char(' '))),
                      qUtf8Printable(unclaimed.join(QLatin1Char(' '))));
            }
        }

        // ── The BDU is the default Base, and nothing else defaults ──────
        // The game's own Defaults blocks in GearConfig.lua name a Base
        // default per class — inb_?03, reb_?03, teb_?03 — and the Enforcer's
        // teb_?03 resolves (through its fova pack) to tcl0_main0_def{_f},
        // the plain battledress the avatar stands in before anything is
        // bought. This page has no class, so the BDU is the one default, per
        // gender, and only when the slot actually lists it: a default naming
        // an absent part would dress the avatar in nothing and say so
        // nowhere. Headgear, Chest and Accessory stay empty by design.
        {
            const QString bdu = QStringLiteral("tcl0_main0_def")
                + (want == Gender::Female ? QStringLiteral("_f") : QString());
            for (const PlayerSlot& sl : s.slotList)
                if (sl.id == QLatin1String("mgo_base")) {
                    for (const CatalogPart& p : sl.parts)
                        if (p.id == bdu) {
                            s.defaults.insert(sl.id, bdu);
                            break;
                        }
                    break;
                }
        }

        // Labelled for what the slot actually HOLDS, not for what the install
        // has: a women's table can be present while naming avf heads this page
        // does not have, and the row was then called "Face Preset" over a list
        // of plain models.
        addSlot(s, QStringLiteral("head"),
                gridApplied ? QStringLiteral("Face Preset")
                            : QStringLiteral("Face"),
                faces);
        // ── One row per hairstyle, not two ──────────────────────────────
        // The archives ship each style twice: "<style>_v00" and
        // "<style>_v0_cov". They are one hairstyle in two states, and listing
        // both put every style in the list twice with nothing to say which was
        // which. Folded here: the plain form is the row, the covered form
        // rides along on it, and the Customize tab picks between them by
        // whether headgear is worn. An install carrying only one of the two —
        // which is what a partial extract usually is — keeps exactly the rows
        // it had before, with nothing to swap to.
        {
            // "<style>_v<N>_cov" is the covered form of "<style>_v0<N>".
            // Measured over the shipped names: 95 "_v00" and 6 "_v01" plain
            // forms, 146 "_v0_cov" and 6 "_v1_cov" covered ones — so a rule
            // written for _v0_cov alone left the six _v1_cov models listed as
            // hairstyles of their own AND left their three real styles with
            // nothing to swap to under a hat.
            static const QRegularExpression covSuffix{
                QStringLiteral("_v([0-9])_cov$")};
            const auto isCovered = [](const QString& k) {
                return covSuffix.match(k).hasMatch();
            };
            const auto uncoveredStem = [](const QString& k) {
                const QRegularExpressionMatch m = covSuffix.match(k);
                if (!m.hasMatch()) return k;
                return k.left(m.capturedStart())
                    + QStringLiteral("_v0") + m.captured(1);
            };
            // TWO PASSES, and it has to be two: a single pass that attached
            // each covered form to a plain row already in the output only
            // worked when the plain form came first in the list. Where it did
            // not, the covered row matched nothing, was not appended either,
            // and vanished from the slot — two of the man's three hairstyles
            // silently lost their covered form that way.
            QVector<CatalogPart> folded;
            QHash<QString, int> plainAt;   // stem -> index in `folded`
            for (const CatalogPart& p : hair)
                if (!isCovered(p.id)) {
                    plainAt.insert(p.id, folded.size());
                    folded.append(p);
                }
            int pairs = 0;
            for (const CatalogPart& p : hair) {
                if (!isCovered(p.id)) continue;
                const int at = plainAt.value(uncoveredStem(p.id), -1);
                if (at < 0) {
                    // No plain partner in this install: it stays a row of its
                    // own, exactly as before.
                    folded.append(p);
                    continue;
                }
                folded[at].coveredFileIdx = p.modelFileIdx;
                ++pairs;
            }
            if (pairs > 0) {
                qInfo("players: %s — %d hairstyle(s) ship a covered form as "
                      "well; each pair is one row and swaps when headgear is "
                      "worn", qUtf8Printable(s.name), pairs);
                hair = folded;
            }
        }
        addSlot(s, QStringLiteral("hair"), QStringLiteral("Hair"), hair);
        addSlot(s, QStringLiteral("horn"), QStringLiteral("Horn"), horn);

        // ── WHERE A HEAD PART HANGS WHEN ITS OWN ROOT IS NOT IN THE WEARER ──
        //
        // Measured over the shipped MGO headgear, by reading each model's root
        // bone and testing it against skl0's 120:
        //
        //   SKL_002_CHEST   hat6, hat8, hat20, hat21, hat22, hat23
        //                   in the skeleton, authored at head height — these
        //                   already aligned themselves and always looked right
        //   SKL_004_HEAD    hat4, hat10, hat11, hat12, hat13, hat17, hat18
        //                   in the skeleton, authored in head-local space
        //   SKL_000_ROOT    hat0, hat1, hat2, hat3, hat7, hat15, hat16, hat19
        //                   NOT in the skeleton at all
        //
        // That last group is every plain cap in the game — the baseball cap,
        // the beret, the skull cap, the busker cap — and with nothing to align
        // to they landed at the model origin, which on this rig is the floor.
        // A hat at the character's feet reads as "positioned incorrectly", and
        // it is the same thing Survive's hat15/hat16/hat30 do for the same
        // reason. The slot supplies the anchor; the rest position still comes
        // from the real skeleton, so nothing here is a guessed offset.
        //
        // WORN-ON-THE-HEAD SLOTS ONLY. Hair and horns are rigged, not worn:
        // every MGO hair model roots at a bone the head already carries, so
        // the bone path above places them and this never runs for them — but
        // giving them the socket anyway would mean that the day one did come
        // through here it landed 10cm above where its bone says, on a
        // different rule from Survive's hair, for no measured reason.
        for (PlayerSlot& sl : s.slotList)
            if (sl.id == QLatin1String("mgo_headgear")
                || sl.id == QLatin1String("mgo_accessory")) {
                sl.anchor = boneCode("SKL_004_HEAD");
                // …and the socket ON that bone. skl0's own .fcnp puts
                // CNP_HEAD at (0, 0.1029, 0.0108) relative to SKL_004_HEAD,
                // and that 10cm is the difference between a cap on the crown
                // and a cap across the mouth. The bone stays as the fallback
                // for an install whose base model ships no connect points.
                sl.anchorCnp = QStringLiteral("CNP_HEAD");
            }

        // The starting face is this page's OWN first head — the first preset
        // where the grid applied, the first model otherwise — same rule as
        // the Survivor's, and what the game itself opens the editor on. The
        // slots were sorted by addSlot above, so parts.first() is that head.
        for (const PlayerSlot& sl : s.slotList)
            if (sl.id == QLatin1String("head") && !sl.parts.isEmpty()) {
                s.defaults.insert(QStringLiteral("head"), sl.parts.first().id);
                break;
            }

        // ── Base, and whether it is safe to hide ────────────────────────
        //
        // HIDING THE BASE IS ONLY SAFE ON A PAGE THAT CAN BE DRESSED. The
        // avatar wears gear and nothing else, and the gear comes from
        // /Assets/mgo/level_asset/config/GearConfig.lua — which a Phantom-Pain-
        // only install does not have. On such an install this page is built
        // anyway (the male avatar's heads and hair live in the TPP tree), the
        // four gear slots come out empty, and hiding the base would leave a
        // head and a hairstyle floating over nothing. So: dressed pages get
        // the real skeleton carrier, hidden; undressable ones keep the bare
        // avatar body visible, which is what this page did before the gear
        // slots existed at all.
        bool dressable = false;
        for (const PlayerSlot& sl : s.slotList)
            if (sl.id.startsWith(QLatin1String("mgo_")) && !sl.parts.isEmpty()) {
                dressable = true;
                break;
            }
        s.baseIsSkeletonOnly = dressable;
        if (dressable && !sklStem.isEmpty()) {
            s.baseStem = sklStem;
            s.baseFileIdx = sklIdx;
        } else if (!bodyStem.isEmpty()) {
            s.baseStem = bodyStem;
            s.baseFileIdx = bodyIdx;
        } else if (!sklStem.isEmpty()) {
            s.baseStem = sklStem;
            s.baseFileIdx = sklIdx;
        }
        // Say out loud which of the three cases this install landed in: a page
        // on the bare-body stand-in is a page whose base pack is missing, and a
        // page that is not dressable is a page with no GearConfig.
        const QString baseName = s.baseStem.isEmpty()
            ? QStringLiteral("MISSING (no skl0 base pack and no avatar body — "
                             "the parts have no skeleton)")
            : s.baseStem;
        qInfo("players: %s — base %s, %s", qUtf8Printable(s.name),
              qUtf8Printable(baseName),
              !dressable
                  ? "SHOWN: this install has no MGO gear table, so there is "
                    "nothing to dress the avatar in and the bare body stands "
                    "in for the character"
              : s.baseStem.startsWith(QLatin1String("skl0"))
                  ? "hidden (the model the game poses and never draws)"
                  : "hidden (stand-in: this install has no skl0 base pack)");

        // The note says what this install actually gave the page, because the
        // answer differs between a TPP-only install and one with MGO mounted.
        QStringList sources;
        sources << (ownFaces ? QStringLiteral("its own face set")
                             : QStringLiteral("the shared avm face set"));
        sources << (ownHair ? QStringLiteral("its own hairstyles")
                            : QStringLiteral("the shared avm hairstyles"));
        s.note = QStringLiteral("%5 The Body combo is the base model (%4), "
                                "shown but not selectable; the DLC suits are "
                                "Snake's own fits and are on his page. This "
                                "page uses %1 and %2.%3")
            .arg(sources.at(0), sources.at(1),
                 (ownFaces && ownHair) ? QString()
                 : QStringLiteral(
                       " The Phantom Pain's own avatar editor is male only — "
                       "one body, one head family, plparts_avatar_man with no "
                       "woman counterpart. Point the browser at the install's "
                       "mgo folder as well and MGO 3's female avatar, with its "
                       "own faces and hairstyles, appears here."),
                 s.baseStem.isEmpty() ? QStringLiteral("not in this install")
                                      : s.baseStem,
                 dressable
                     ? QStringLiteral(
                           "Dressed entirely by the game's four gear "
                           "categories, on an animation base the game never "
                           "draws.")
                     : QStringLiteral(
                           "This install has no MGO gear table "
                           "(GearConfig.lua), so there is nothing to dress "
                           "this avatar in and the bare body is shown as the "
                           "character. Point the browser at the install's mgo "
                           "folder for the wardrobe."));
        if (!tones.isEmpty())
            s.note += QStringLiteral(" The game ships %1 skin tones per body "
                                     "(lightest to darkest); choosing one is "
                                     "not wired up yet.")
                          .arg(tones.size());

        if (!s.slotList.isEmpty()) m_subjects.append(s);
    }

    // ── The named characters MGO ships as playable skins ─────────────────────
    //
    // Ocelot, Quiet and Snake are complete characters in MGO3, not avatars: one
    // model each, no slots, nothing to configure. They were reachable only by
    // hunting for their model stem in the "every other humanoid" category,
    // which is the wrong drawer — a player thinks of them by name.
    //
    // A subject with no slots is deliberate and the composer already handles
    // it: baseIsSkeletonOnly stays false, so the base model IS the character
    // and the page shows it with nothing to change. Anything that assumes a
    // subject has rows has to cope with that anyway, because a partial install
    // can empty any page.
    {
        struct SkinRule { const char* stem; const char* name; Gender g; };
        // Named from the model stems MGO actually ships. qui0_main0_MGO is
        // spelled with the game's own suffix — it is a different model from
        // TPP's qui0_main0_def and only the MGO one belongs here.
        static const SkinRule kSkins[] = {
            {"oce0_main1_def",  "Ocelot",        Gender::Male},
            {"qui0_main0_mgo",  "Quiet",         Gender::Female},
            {"sna0_main4_def",  "Snake (MGO)",   Gender::Male},
        };
        for (const SkinRule& r : kSkins) {
            const int idx = models.value(QLatin1String(r.stem), -1);
            if (idx < 0) continue;   // not in this install
            PlayerSubject sk;
            sk.id = QString::fromLatin1(r.stem);
            sk.name = QString::fromLatin1(r.name);
            sk.game = GameId::Mgo;
            sk.gender = r.g;
            sk.baseStem = QString::fromLatin1(r.stem);
            sk.baseFileIdx = idx;
            sk.baseIsSkeletonOnly = false;
            sk.variantSlot.clear();
            sk.note = QStringLiteral(
                "A complete MGO3 character skin — one model, worn as-is. These "
                "have no customization of their own: the gear slots belong to "
                "the avatar, not to a named character.");
            m_subjects.append(sk);
        }
    }
}

// ── MGSV: The Phantom Pain ──────────────────────────────────────────────────
void PlayerCatalog::buildTpp(const QHash<QString, int>& models)
{
    if (models.isEmpty()) return;
    // Snake. His uniforms are sna<N>_main<N>_def; the gendered sna4/5/7_ply*
    // models belong to the Diamond Dogs player, not to him.
    PlayerSubject snake;
    snake.id = QStringLiteral("tpp_sna");
    snake.name = QStringLiteral("Snake (Venom)");
    snake.game = GameId::Tpp;
    snake.gender = Gender::Any;
    snake.variantSlot = QStringLiteral("body");
    snake.note = QStringLiteral(
        "Uniform, head option and arm — the three things the game lets you "
        "change on Snake. Uniform names come from the development list. The "
        "DLC suits (dl*_main0_def) are his too: each ships up to three fits — "
        "his own _main0, and the MGO avatar's _plym0 / _plyf0 — and only the "
        "_main0 is cut for him.");
    QVector<CatalogPart> bodies, faces, arms;
    // THE DLC SUITS ARE SNAKE'S. Each dl* family ships up to three fits of
    // itself: dla0_main0_def is Snake's, dla0_plym0_def the male avatar's,
    // dlc0_plyf0_def the female's. The avatar fits used to be the MGO page's
    // "Outfit" row and are gone with it (the MGO avatar is dressed by its gear
    // slots, on a base it never draws — see buildMgo); the _main0 fit is the
    // one cut for Snake and belongs on his Uniform list.
    //
    // Scoped by PATH, not by the stem alone: "dl" is two letters and matches
    // plenty that is not a character. A uniform is a character model, so the
    // file has to sit under a /chara/ tree — the /chara/ half of the MGO gear
    // join's gearPathOk, for the same reason. It does NOT need that test's
    // "and not /Assets/ssd/" half: buildTpp is handed the TPP map alone, and
    // ArchiveIndex::gameOf files an /Assets/ssd/ path under Survive, so a
    // Survive model cannot be in this map to begin with.
    const auto& allFiles = ArchiveIndex::instance().files();
    const auto isCharaModel = [&allFiles](int fileIdx) {
        return fileIdx >= 0 && fileIdx < allFiles.size()
            && allFiles[fileIdx].path.contains(QLatin1String("/chara/"),
                                               Qt::CaseInsensitive);
    };
    static const QRegularExpression dlcFamily{QStringLiteral("^dl[a-z][0-9]$")};
    int dlcSuits = 0;
    for (auto it = models.constBegin(); it != models.constEnd(); ++it) {
        const QString& k = it.key();
        const bool isSna = k.startsWith(QLatin1String("sna"));
        const bool isDlc = dlcFamily.match(k.section(QLatin1Char('_'), 0, 0))
                               .hasMatch()
            && isCharaModel(it.value());
        if (!isSna && !isDlc) continue;
        if (k.contains(QLatin1String("_ply")) || k.contains(QLatin1String("_ene")))
            continue;   // the FOB player's models, and the enemy variants
        // A patch chunk's copy of the same asset. Every one of the 1,209
        // _patched stems in the shipped set has a plain twin, so listing both
        // put 61 rows in a Uniform combo that holds 31 uniforms — and because
        // both resolve to the same development-list name, thirty of them were
        // duplicate rows with identical headlines. The derived pass below and
        // both other catalogues already fold these; this loop did not.
        if (k.endsWith(QLatin1String("_patched"))) continue;
        // A DLC family's female re-fit (…_main0_def_f) is not Snake's.
        if (isDlc && k.endsWith(QLatin1String("_def_f"))) continue;
        // ARMS BEFORE BODIES. sna0_main4_arm_cov and sna0_main5_arm_cov carry
        // both tokens, and testing _main first put two arm covers in the
        // Uniform combo — headlined with sna0's suit name, so selecting one
        // replaced the whole character with a sleeve.
        if (!isDlc
            && (k.contains(QLatin1String("_arm"))
                || k.contains(QLatin1String("_rkt")))) {
            arms.append(makePart(QStringLiteral("arm"), k, it.value()));
        } else if (k.contains(QLatin1String("_main"))) {
            bodies.append(makePart(QStringLiteral("body"), k, it.value()));
            if (isDlc) ++dlcSuits;
        } else if (isDlc) {
            continue;   // a DLC family contributes its uniform and nothing else
        } else if (k.contains(QLatin1String("_face"))) {
            faces.append(makePart(QStringLiteral("face"), k, it.value()));
        }
    }
    // Sort before taking the first: these come out of a QHash, so "first"
    // would otherwise be whichever stem the hash happened to yield.
    std::sort(bodies.begin(), bodies.end(),
              [](const CatalogPart& a, const CatalogPart& b) { return a.id < b.id; });
    if (!bodies.isEmpty())
        qInfo("players: Snake — %lld uniform(s), of which %d DLC suit(s)",
              qint64(bodies.size()), dlcSuits);
    // His own first uniform, not merely the first in the list: "dla0" sorts
    // ahead of "sna0", so taking bodies.first() after the DLC suits joined the
    // list would have opened his page in a DLC suit.
    for (const CatalogPart& b : bodies)
        if (b.id.startsWith(QLatin1String("sna"))) { snake.baseStem = b.id; break; }
    if (snake.baseStem.isEmpty() && !bodies.isEmpty())
        snake.baseStem = bodies.first().id;
    snake.baseFileIdx = models.value(snake.baseStem, -1);
    addSlot(snake, QStringLiteral("body"), QStringLiteral("Uniform"), bodies);
    addSlot(snake, QStringLiteral("face"), QStringLiteral("Head Option"), faces);
    addSlot(snake, QStringLiteral("arm"), QStringLiteral("Arm"), arms);
    if (!snake.slotList.isEmpty()) m_subjects.append(snake);

    // The Diamond Dogs player — the FOB soldier, gender-split by the game's own
    // pack names (plparts_dd_male / plparts_dd_female).
    for (int gi = 0; gi < 2; ++gi) {
        const Gender want = gi == 0 ? Gender::Male : Gender::Female;
        PlayerSubject s;
        s.id = gi == 0 ? QStringLiteral("tpp_dd_m") : QStringLiteral("tpp_dd_f");
        s.name = gi == 0 ? QStringLiteral("Diamond Dogs Soldier — Male")
                         : QStringLiteral("Diamond Dogs Soldier — Female");
        s.variantSlot = QStringLiteral("body");
        s.game = GameId::Tpp;
        s.gender = want;
        s.note = QStringLiteral(
            "The FOB player. Outfits are the _plyf0/_plym0 models the game "
            "packs as plparts_dd%1_battledress / _parasite / _venom / "
            "_swimwear.").arg(gi == 0 ? QStringLiteral("m") : QStringLiteral("f"));
        QVector<CatalogPart> outfits, heads;
        for (auto it = models.constBegin(); it != models.constEnd(); ++it) {
            const QString& k = it.key();
            if (k.endsWith(QLatin1String("_patched"))) continue;   // see above
            if (k.contains(QLatin1String("_ply"))) {
                // A dl*_ply* model is a DLC suit's AVATAR fit. Nobody wears
                // those any more — the MGO avatar is dressed by its gear
                // slots, and Snake takes the suits' _main0 fits — and they
                // were never this character's: the FOB soldier wears The
                // Phantom Pain's own player outfits.
                if (k.startsWith(QLatin1String("dl"))) continue;
                if (genderOfPart(k) == want)
                    outfits.append(makePart(QStringLiteral("body"), k, it.value()));
            } else if (k.startsWith(QLatin1String("dds"))) {
                if (k.contains(QLatin1String("_eqhd"))
                    || k.contains(QLatin1String("_head")))
                    heads.append(makePart(QStringLiteral("head"), k, it.value()));
                else if (k.contains(QLatin1String("_main")))
                    outfits.append(makePart(QStringLiteral("body"), k, it.value()));
            }
        }
        std::sort(outfits.begin(), outfits.end(),
                  [](const CatalogPart& a, const CatalogPart& b) { return a.id < b.id; });
        if (!outfits.isEmpty()) {
            // The DEFAULT fit, not the alphabetically first: sorted, the list
            // starts dds0_main0_cov and the page opened on a cover model. Same
            // rule the derived characters use, and the same reason.
            s.baseStem = outfits.first().id;
            for (const CatalogPart& o : outfits)
                if (o.id.endsWith(QLatin1String("_def"))) { s.baseStem = o.id; break; }
            s.baseFileIdx = models.value(s.baseStem, -1);
        }
        addSlot(s, QStringLiteral("body"), QStringLiteral("Outfit"), outfits);
        addSlot(s, QStringLiteral("head"), QStringLiteral("Headgear"), heads);
        if (!s.slotList.isEmpty()) m_subjects.append(s);
    }
}

// ── The Phantom Pain: everyone else ─────────────────────────────────────────
//
// Snake and the Diamond Dogs soldier are hand-built above because their slots
// are named by the game's own packs. Nobody else is — and there are ninety-odd
// other character families in the archives, several of which the game dresses
// as thoroughly as it dresses Snake. Quiet ships thirteen bodies, a hood, a
// headpiece and her sling; Huey five hoods and five items; Miller five bodies
// and three head pieces; the Soviet and PF soldiers six head pieces each.
// Every one of them was reachable only as a flat model list.
//
// So this reads the SECOND TOKEN of the model name, which is Fox's own part
// vocabulary and is consistent across the whole set. Measured over the 2,070
// character models the user's install carries, with the avatar excluded:
//
//   main 252   eqhd 49   face 32   eqit 16   arm 14   dummy 11   hood 8
//   base 8     ephd 7    coat 7    eqbd 6    head 5   body 5     hand 4
//   rkt 4      berd 3    txdm 3    dcoy 3    …and a long tail
//
// A token maps to a slot; a body (`main`) is the character itself and becomes
// the Body combo. Tokens that name something that is not a wearable part —
// dummies, decoys, marker helpers, texture-only entries — are listed at the
// bottom under "Other parts" rather than dropped, because "the archives hold
// this and we do not know what it is" is a true statement and hiding it is not.
//
// The NAMES are ours where we are sure of them and the family code where we
// are not, and the code is in the label either way, so a wrong guess can never
// hide what the asset actually is. There is no character-name table in the
// shipped data — the .lng2 tables carry UI and story text and no model names —
// which was checked before this was written.
void PlayerCatalog::buildTppCharacters(const QHash<QString, int>& models)
{
    if (models.isEmpty()) return;

    // Token -> slot. The LABEL is a property of the slot, not of the token
    // that reached it: several tokens land in one slot, and labelling per
    // token meant whichever token a QHash happened to yield last named the
    // row — Quiet's Item row read "Lead" on one run and "Item" on the next.
    struct SlotRule { const char* token; const char* id; };
    static const SlotRule kRules[] = {
        {"face", "face"},      {"head", "head"},     {"hair", "hair"},
        {"berd", "beard"},     {"ebrw", "brow"},     {"horn", "horn"},
        {"eqhd", "head_equipment"}, {"ephd", "head_equipment"},
        {"band", "band"},      {"bdn", "band"},
        {"hood", "hood"},      {"hooh", "hood"},     {"coat", "coat"},
        {"arm", "arm"},        {"arms", "arm"},
        {"rkt", "arm"},        {"rocket", "arm"},    {"hand", "hand"},
        {"eqbd", "torso"},     {"epbd", "torso"},    {"eqbp", "torso"},
        {"cloth", "torso"},    {"clth", "torso"},    {"clva", "torso"},
        {"eqlg", "leg"},       {"legs", "leg"},
        {"pant", "leg"},       {"wpan", "leg"},
        {"eqit", "accessory"}, {"epit", "accessory"}, {"item", "accessory"},
        {"eqpd", "accessory"}, {"eqth", "accessory"}, {"watch", "accessory"},
        {"necktie", "accessory"}, {"ntie", "accessory"},
        {"sling", "sling"},    {"slng", "sling"},    {"rope", "sling"},
        {"lead", "lead"},
    };
    struct SlotName { const char* id; const char* label; };
    static const SlotName kSlotNames[] = {
        {"body", "Body"},      {"face", "Face"},     {"head", "Head"},
        {"hair", "Hair"},      {"beard", "Beard"},   {"brow", "Eyebrows"},
        {"horn", "Horn"},      {"head_equipment", "Headgear"},
        {"band", "Bandana"},   {"hood", "Hood"},     {"coat", "Coat"},
        {"arm", "Arm"},        {"hand", "Hands"},    {"torso", "Body gear"},
        {"leg", "Legs"},       {"accessory", "Item"}, {"sling", "Sling"},
        {"lead", "Lead"},      {"other", "Other parts"},
    };
    const auto labelFor = [](const QString& id) {
        for (const SlotName& n : kSlotNames)
            if (id == QLatin1String(n.id)) return QString::fromLatin1(n.label);
        return id;
    };
    // Not parts: helpers, decoys and texture-only entries. Everything NOT in
    // this list and not in the table above still reaches the page, under
    // "Other parts".
    static const char* const kNotParts[] = {
        "dummy", "dumm", "dcoy", "marker", "mk", "texture", "tex", "stage",
        "fova", "patch", "pacth", "color", "variationtex", "shdw", "demo",
    };
    // Built by name above, or not a character at all.
    static const char* const kSkipFamily[] = {
        "sna", "dds",                       // Snake and the DD soldier
        "avm", "avf", "acce",               // the avatar and its accessories
        "dla", "dlb", "dlc", "dld", "dle",  // DLC suits — Snake's uniforms
        "dum", "tex", "olm", "pvz",         // dummies, markers, mission helpers
    };
    // Ours, and only where we are sure. The code stays in the label, so a
    // label that is wrong is still traceable to the asset it names.
    struct NameRule { const char* code; const char* name; };
    static const NameRule kNames[] = {
        {"qui", "Quiet"},        {"oce", "Ocelot"},
        {"kaz", "Miller (Kaz)"}, {"hyu", "Huey"},
        {"cdt", "Code Talker"},  {"vol", "Volgin"},
        {"paz", "Paz"},          {"chi", "Chico"},
        {"dog", "D-Dog"},        {"hrs", "D-Horse"},
    };

    // Scoped by PATH. The token vocabulary is not unique to characters: a
    // weapon is <family><n>_main<n>_def too, so without this the Character
    // page grew rows called AM, BU, HG and PT — the assault rifles, the
    // bullpups, the handguns and the pistol parts, each offered as a person
    // to dress. A character model lives under a /chara/ tree; buildTpp uses
    // the same test on the DLC families, for the same reason.
    const auto& allFiles = ArchiveIndex::instance().files();
    const auto isCharaModel = [&allFiles](int fileIdx) {
        return fileIdx >= 0 && fileIdx < allFiles.size()
            && allFiles[fileIdx].path.contains(QLatin1String("/chara/"),
                                               Qt::CaseInsensitive);
    };

    const auto tokenOf = [](const QString& stem) {
        QString t = stem.section(QLatin1Char('_'), 1, 1);
        while (!t.isEmpty() && t.back().isDigit()) t.chop(1);
        return t;
    };
    const auto familyOf = [](const QString& stem) {
        const QString first = stem.section(QLatin1Char('_'), 0, 0);
        QString f;
        for (const QChar c : first) {
            if (!c.isLetter()) break;
            f.append(c);
        }
        return f;
    };

    // family -> slot id -> parts, plus the labels the rules chose.
    QHash<QString, QHash<QString, QVector<CatalogPart>>> byFamily;
    QHash<QString, int> bodyCount;
    int folded = 0;
    for (auto it = models.constBegin(); it != models.constEnd(); ++it) {
        const QString& stem = it.key();
        // A patched copy is the same asset from a patch chunk — every one of
        // them in the shipped set has a plain twin — so listing both would
        // double every row for no choice gained.
        if (stem.endsWith(QLatin1String("_patched"))) { ++folded; continue; }
        // …and a model whose LAST token says dummy is a placeholder whatever
        // its part token says: qui0_main0_dummy is not a thirteenth body.
        const QString last = stem.section(QLatin1Char('_'), -1);
        if (last == QLatin1String("dummy") || last == QLatin1String("dumm")
            || last == QLatin1String("dcoy"))
            continue;
        if (!isCharaModel(it.value())) continue;
        const QString fam = familyOf(stem);
        if (fam.size() < 2) continue;
        bool skip = false;
        for (const char* s2 : kSkipFamily)
            if (fam == QLatin1String(s2)) { skip = true; break; }
        if (skip) continue;

        const QString tok = tokenOf(stem);
        if (tok.isEmpty()) continue;
        if (tok == QLatin1String("main") || tok == QLatin1String("body")) {
            byFamily[fam][QStringLiteral("body")].append(
                makePart(QStringLiteral("body"), stem, it.value()));
            ++bodyCount[fam];
            continue;
        }
        QString id;
        for (const SlotRule& r : kRules)
            if (tok == QLatin1String(r.token)) { id = QLatin1String(r.id); break; }
        if (id.isEmpty()) {
            bool junk = false;
            for (const char* j : kNotParts)
                if (tok == QLatin1String(j)) { junk = true; break; }
            if (junk) continue;
            id = QStringLiteral("other");
        }
        byFamily[fam][id].append(makePart(id, stem, it.value()));
    }

    int added = 0, parts = 0;
    QStringList names;
    for (auto f = byFamily.constBegin(); f != byFamily.constEnd(); ++f) {
        const QString& fam = f.key();
        // A character needs a BODY to stand on and at least one choice
        // anywhere. One lone model is a model, not a character, and it is
        // already in the every-other-humanoid list below the divider.
        if (bodyCount.value(fam, 0) == 0) continue;
        int total = 0;
        for (auto sl = f->constBegin(); sl != f->constEnd(); ++sl)
            total += sl->size();
        if (total < 2) continue;

        PlayerSubject s;
        s.id = QStringLiteral("tpp_") + fam;
        QString proper;
        for (const NameRule& n : kNames)
            if (fam == QLatin1String(n.code)) { proper = QLatin1String(n.name); break; }
        // The family code is on the row's second line already, so the label is
        // the name alone — "Miller (Kaz) (kaz)" was the code said twice.
        s.name = proper.isEmpty() ? fam.toUpper() : proper;
        s.game = GameId::Tpp;
        s.gender = Gender::Any;
        s.variantSlot = QStringLiteral("body");
        s.derived = true;
        s.note = QStringLiteral(
            "Slots read from the part token in each model's name (%1). The "
            "name above is ours where we recognise the family code and the "
            "code itself where we do not — the game ships no character-name "
            "table. Appearance variations come from each model's own .fv2.")
            .arg(fam);

        for (auto sl = f->constBegin(); sl != f->constEnd(); ++sl)
            addSlot(s, sl.key(), labelFor(sl.key()), sl.value());
        // The body the page OPENS on. Sorted order alone is not enough: the
        // parts sort by stem, so Quiet's page opened on qui0_main0_bld — her
        // bloodied model — because "bld" sorts before "def". A model whose
        // last token is `def` is the default fit by Fox's own convention, so
        // that is what the page opens on when the family ships one.
        for (const PlayerSlot& sl : s.slotList) {
            if (sl.id != QLatin1String("body") || sl.parts.isEmpty()) continue;
            const CatalogPart* pick = &sl.parts.first();
            for (const CatalogPart& c : sl.parts)
                if (c.id.endsWith(QLatin1String("_def"))) { pick = &c; break; }
            s.baseStem = pick->id;
            s.baseFileIdx = pick->modelFileIdx;
            break;
        }
        if (s.slotList.isEmpty() || s.baseFileIdx < 0) continue;
        m_subjects.append(s);
        ++added;
        parts += total;
        names << (proper.isEmpty() ? fam : proper);
    }
    names.sort();
    if (added)
        qInfo("players: %d other TPP character(s), %d part(s)%s — %s", added,
              parts,
              folded ? qUtf8Printable(QStringLiteral(" (%1 patched duplicate(s) "
                                                 "folded)").arg(folded))
                     : "",
              qUtf8Printable(names.join(QStringLiteral(", "))));
}

// ── MGSV: Ground Zeroes ─────────────────────────────────────────────────────
void PlayerCatalog::buildGz(const QHash<QString, int>& models)
{
    if (models.isEmpty()) return;
    PlayerSubject s;
    s.id = QStringLiteral("gz_sna");
    s.name = QStringLiteral("Snake — Ground Zeroes");
    s.game = GameId::GroundZeroes;
    s.gender = Gender::Any;
    s.variantSlot = QStringLiteral("body");
    s.note = QStringLiteral(
        "Ground Zeroes ships its characters as complete models with no part "
        "slots, so this is a body list rather than a customizer.");
    QVector<CatalogPart> bodies;
    for (auto it = models.constBegin(); it != models.constEnd(); ++it)
        if (it.key().startsWith(QLatin1String("sna")))
            bodies.append(makePart(QStringLiteral("body"), it.key(), it.value()));
    if (bodies.isEmpty()) return;
    std::sort(bodies.begin(), bodies.end(),
              [](const CatalogPart& a, const CatalogPart& b) { return a.id < b.id; });
    s.baseStem = bodies.first().id;
    s.baseFileIdx = bodies.first().modelFileIdx;
    addSlot(s, QStringLiteral("body"), QStringLiteral("Body"), bodies);
    m_subjects.append(s);
}

QString PlayerCatalog::describe() const
{
    if (m_subjects.isEmpty())
        return QStringLiteral("no player characters in the configured folders");
    int slotCount = 0, partCount = 0;
    QStringList games;
    for (const PlayerSubject& s : m_subjects) {
        slotCount += s.slotList.size();
        for (const PlayerSlot& sl : s.slotList) partCount += sl.parts.size();
        const QString g = QString::fromLatin1(gameShortName(s.game));
        if (!games.contains(g)) games << g;
    }
    return QStringLiteral("%1 player character(s) across %2 — %3 slot(s), "
                          "%4 part(s)")
        .arg(m_subjects.size())
        .arg(games.join(QStringLiteral("/")))
        .arg(slotCount)
        .arg(partCount);
}


// ── The builder source ──────────────────────────────────────────────────────
//
// Contextual by construction: the slot list and the parts in it both come from
// the chosen subject, so switching from Snake to a Survive survivor rebuilds
// the panel into that character's eight slots, and every slot only ever offers
// parts whose own name says they belong to that character.
BuilderSource PlayerCatalog::builderSource() const
{
    BuilderSource src;
    src.subjectLabel = QStringLiteral("Character");
    src.variantLabel = QStringLiteral("Body");
    src.emptyHint = QStringLiteral(
        "No player characters in the configured folders. Check the game "
        "toggles above, and that a game folder is set in Settings.");

    // m_subjects is already in display order — player characters first, then
    // the derived ones, each alphabetically — because build() sorts it there
    // rather than here. One order, used by the panel and by the dump alike.
    for (const PlayerSubject& s : m_subjects) {
        CatalogSubject cs;
        cs.id = s.id;
        // Two groups per game, and the caption says which claim each makes.
        cs.groupName = s.derived
            ? QStringLiteral("%1 — other characters (slots from model names)")
                  .arg(QString::fromLatin1(gameLongName(s.game)))
            : QStringLiteral("%1 — player characters")
                  .arg(QString::fromLatin1(gameLongName(s.game)));
        cs.displayName = s.name;
        cs.hiddenVariant = s.baseIsSkeletonOnly;
        // The "variants" of a player character are the bodies of its first
        // slot — the uniform list — so the panel's Body combo is the outfit
        // picker without any special-casing.
        for (const PlayerSlot& sl : s.slotList) {
            if (sl.id != s.variantSlot) continue;
            for (const CatalogPart& p : sl.parts) {
                CatalogPart v = p;
                v.slot = QStringLiteral("body");
                cs.variants.append(v);
            }
            break;
        }
        // THE CHARACTER'S OWN BODY FIRST. The panel opens a subject on variant
        // 0, and the list is sorted by stem — so Snake's page opened in a DLC
        // suit ("dla0" sorts before "sna0") and Quiet's opened on her bloodied
        // model ("bld" before "def"), both of which the catalogue had already
        // worked out and then had no way to say. baseStem is that answer;
        // moving it to the front is how the panel hears it.
        for (int i = 1; i < cs.variants.size(); ++i)
            if (cs.variants[i].id == s.baseStem) {
                cs.variants.move(i, 0);
                break;
            }
        if (cs.variants.isEmpty() && s.baseFileIdx >= 0) {
            CatalogPart v;
            v.slot = QStringLiteral("body");
            v.id = s.baseStem;
            v.displayName = s.baseStem;
            v.modelFileIdx = s.baseFileIdx;
            cs.variants.append(v);
        }
        // The variant slot is the Body combo, not a part slot — counting it
        // made every subject report one slot more than the panel shows.
        cs.ownPartCount = s.slotList.size();
        for (const PlayerSlot& sl : s.slotList)
            if (sl.id == s.variantSlot) { --cs.ownPartCount; break; }
        src.subjects.append(cs);
    }

    // Every slot name any player character uses, in a stable order — the panel
    // reads this when a category has no contextual hook, and the export code
    // uses it for ordering.
    static const char* const kOrder[] = {
        "head", "face", "hair", "head_equipment", "accessory", "hats",
        "glasses", "horn", "body", "torso", "chest_rig", "arm", "leg",
    };
    QStringList all;
    for (const char* n : kOrder) {
        const QString slot = QLatin1String(n);
        for (const PlayerSubject& s : m_subjects) {
            bool has = false;
            for (const PlayerSlot& sl : s.slotList)
                if (sl.id == slot) { has = true; break; }
            if (has) { all << slot; break; }
        }
    }
    src.slotNames = all;

    src.slotsForSubject = [](const QString& subjectId) -> QStringList {
        const PlayerSubject* s = PlayerCatalog::instance().find(subjectId);
        if (!s) return {};
        QStringList out;
        for (const PlayerSlot& sl : s->slotList)
            if (sl.id != s->variantSlot) out << sl.id;
        return out;
    };
    src.partsForSubject = [](const QString& subjectId,
                             const QString& slot) -> QVector<CatalogPart> {
        const PlayerSubject* s = PlayerCatalog::instance().find(subjectId);
        if (!s) return {};
        for (const PlayerSlot& sl : s->slotList)
            if (sl.id == slot) return sl.parts;
        return {};
    };
    src.slotLabelFor = [](const QString& subjectId,
                          const QString& slot) -> QString {
        const PlayerSubject* s = PlayerCatalog::instance().find(subjectId);
        if (s)
            for (const PlayerSlot& sl : s->slotList)
                if (sl.id == slot) return sl.label;
        return {};
    };
    src.defaultsFor = [](const QString& subjectId) -> QHash<QString, int> {
        QHash<QString, int> out;
        const PlayerSubject* s = PlayerCatalog::instance().find(subjectId);
        if (!s) return out;
        for (auto it = s->defaults.constBegin(); it != s->defaults.constEnd(); ++it)
            for (const PlayerSlot& sl : s->slotList) {
                if (sl.id != it.key()) continue;
                for (const CatalogPart& p : sl.parts)
                    if (p.id == it.value()) { out.insert(sl.id, p.modelFileIdx); break; }
                break;
            }
        return out;
    };
    src.variantHidden = [](const QString& subjectId) -> bool {
        const PlayerSubject* s = PlayerCatalog::instance().find(subjectId);
        return s && s->baseIsSkeletonOnly;
    };
    src.anchorBoneFor = [](const QString& subjectId,
                           const QString& slot) -> quint32 {
        const PlayerSubject* s = PlayerCatalog::instance().find(subjectId);
        if (s)
            for (const PlayerSlot& sl : s->slotList)
                if (sl.id == slot) return sl.anchor;
        return 0;
    };
    src.anchorCnpFor = [](const QString& subjectId,
                          const QString& slot) -> QString {
        const PlayerSubject* s = PlayerCatalog::instance().find(subjectId);
        if (s)
            for (const PlayerSlot& sl : s->slotList)
                if (sl.id == slot) return sl.anchorCnp;
        return {};
    };
    src.subjectNote = [](const QString& subjectId) -> QString {
        const PlayerSubject* s = PlayerCatalog::instance().find(subjectId);
        if (!s) return {};
        return QStringLiteral("%1 · %2 · %3")
            .arg(QString::fromLatin1(gameLongName(s->game)),
                 QString::fromLatin1(genderName(s->gender)), s->note);
    };

    // Non-contextual fallbacks, so the shared panel code never has to test.
    src.partsFor = [](const QString&) -> QVector<CatalogPart> { return {}; };
    src.ownPartFor = [](const QString&, const QString&) -> const CatalogPart* {
        return nullptr;
    };
    // Appearance variations are the same .fv2 tables the character browser
    // reads, so borrow its accessor — once, not once per call.
    src.variationsFor = [](const QString& stem) -> QVector<CatalogVariation> {
        static const BuilderSource chars = CharacterCatalog::instance().builderSource();
        return chars.variationsFor ? chars.variationsFor(stem)
                                   : QVector<CatalogVariation>();
    };
    return src;
}

}  // namespace fox
