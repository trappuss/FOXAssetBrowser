// AnimCatalog.cpp — see AnimCatalog.h.
#include "index/AnimCatalog.h"

#include <QElapsedTimer>
#include <QSet>
#include <algorithm>
#include <climits>

#include "fox/FmdlFile.h"
#include "fox/FrigFile.h"
#include "fox/MtarFile.h"
#include "index/ArchiveIndex.h"
#include "index/GameId.h"

namespace fox {
namespace {

// ── Categories ───────────────────────────────────────────────────────────────
//
// Tokens matched ANYWHERE in the clip name, not at a fixed position. Fox's
// naming puts the action in different slots depending on whether the clip
// carries a stance letter, and a clip is often two things at once
// ("wkrg_s_rol_dm_big_b" is a roll AND a damage reaction) — so the table is
// tried in PRIORITY ORDER and the first category with a token present wins.
// Damage before locomotion is deliberate: a roll that ends in being knocked
// down is filed under what happened to the character, which is how someone
// looking for it would think of it.
//
// Every token here appears in the shipped data. Nothing is listed on the
// strength of what Fox "probably" calls something.
struct CategoryRule {
    AnimCategory cat;
    const char* tokens;   // space-separated
};

const CategoryRule kRules[] = {
    {AnimCategory::Facial,
     "deform face facial mouth eyebrow"},
    {AnimCategory::Damage,
     "dm dam die dead damage shock fls blw hit ele"},
    {AnimCategory::Cqc,
     "cqc atk thrw throw punch mach slh prs hag grab bite bark"},
    {AnimCategory::ReadyMove,
     "rdyrun rdywlk rdywalk"},
    {AnimCategory::Weapon,
     "fre fre0 fre1 fre2 fre3 fre0ed rld reload rdy set aw0 aw1 aw2 ful c4 hgn"},
    {AnimCategory::Locomotion,
     "wk wlk walk rn run dsh dash trot trt canter gllp jmp tn turn dh stp step "
     "mov rol roll dve ledge clf60 mlp"},
    {AnimCategory::Idle,
     "idl idle wait slp sns wt cnt"},
    {AnimCategory::Interaction,
     "dor door pat rp psh rid pic ene heli opn open"},
};

// One shared token set per rule, built once. A QSet lookup per token beats
// re-splitting a string literal for every one of 2,855 clips.
const QVector<QSet<QString>>& ruleSets()
{
    static const QVector<QSet<QString>> sets = [] {
        QVector<QSet<QString>> out;
        for (const CategoryRule& r : kRules) {
            QSet<QString> s;
            for (const QString& t :
                 QString::fromLatin1(r.tokens).split(QLatin1Char(' '),
                                                     Qt::SkipEmptyParts))
                s.insert(t);
            out.append(s);
        }
        return out;
    }();
    return sets;
}

// ── The readable expansion ───────────────────────────────────────────────────
//
// OURS, not the game's — there is no in-game translation of motion names, which
// was checked against the whole .lng2 catalogue before this was written. Only
// well-attested tokens are listed; anything absent passes through unchanged, so
// the expansion can add readability but never invent meaning.
struct Word { const char* token; const char* text; };

const Word kWords[] = {
    // Locomotion
    {"wk", "walk"},      {"wlk", "walk"},     {"walk", "walk"},
    {"rn", "run"},       {"run", "run"},      {"dsh", "dash"},
    {"dash", "dash"},    {"trot", "trot"},    {"trt", "trot"},
    {"canter", "canter"},{"gllp", "gallop"},  {"jmp", "jump"},
    {"tn", "turn"},      {"turn", "turn"},    {"rol", "roll"},
    {"roll", "roll"},    {"dve", "dive"},     {"stp", "step"},
    {"step", "step"},    {"mov", "move"},     {"ledge", "ledge"},
    // Weapon
    {"fre", "fire"},     {"fre0", "fire"},    {"fre1", "fire 1"},
    {"fre2", "fire 2"},  {"fre3", "fire 3"},  {"fre0ed", "fire end"},
    {"rld", "reload"},
    {"reload", "reload"},{"rdy", "ready"},    {"set", "set"},
    {"rdyrun", "ready run"}, {"rdywlk", "ready walk"},
    {"rdywalk", "ready walk"}, {"ful", "fulton"}, {"c4", "C4"},
    {"hgn", "handgun"},
    // Combat
    {"cqc", "CQC"},      {"atk", "attack"},   {"thrw", "throw"},
    {"throw", "throw"},  {"punch", "punch"},  {"slh", "slash"},
    {"mach", "machete"}, {"grab", "grab"},    {"bite", "bite"},
    // Damage
    {"dm", "damage"},    {"dam", "damage"},   {"damage", "damage"},
    {"die", "die"},      {"shock", "shock"},  {"fls", "fall"},
    {"blw", "blown back"}, {"hit", "hit"},
    // Idle and interaction
    {"idl", "idle"},     {"idle", "idle"},    {"wait", "wait"},
    {"slp", "sleep"},    {"sns", "sense"},    {"dor", "door"},
    {"door", "door"},    {"pat", "pat"},      {"psh", "push"},
    {"rid", "ride"},     {"pic", "pick up"},  {"bark", "bark"},
    {"heli", "helicopter"}, {"opn", "open"},  {"open", "open"},
    {"deform", "deform"},
    // Phase and direction
    {"st", "start"},     {"ed", "end"},       {"lp", "loop"},
    {"l", "left"},       {"r", "right"},      {"f", "forward"},
    {"b", "back"},       {"u", "up"},         {"d", "down"},
    {"dwn", "down"},     {"ris", "rise"},     {"short", "short"},
    {"long", "long"},    {"fst", "fast"},     {"mid", "mid"},
    {"big", "big"},      {"sml", "small"},
};

const QHash<QString, QString>& wordMap()
{
    static const QHash<QString, QString> m = [] {
        QHash<QString, QString> h;
        h.reserve(int(sizeof(kWords) / sizeof(kWords[0])) * 2);
        for (const Word& w : kWords)
            h.insert(QString::fromLatin1(w.token), QString::fromLatin1(w.text));
        return h;
    }();
    return m;
}

// "l090" / "r45" / "090" — a direction with an angle, or a bare angle. Fox
// writes turn targets this way and they are the most common unexpanded token
// left once the word list has run.
bool expandAngle(const QString& tok, QString* out)
{
    if (tok.isEmpty()) return false;
    int i = 0;
    QString dir;
    QString sign;
    // Fox writes some turn targets as a signed heading ("tn_-135_r"), so a
    // leading minus is part of the number rather than a separate token.
    if (tok.at(0) == QLatin1Char('-')) { sign = QStringLiteral("-"); i = 1; }
    if (i >= tok.size()) return false;
    const QChar c0 = tok.at(i);
    if (c0 == QLatin1Char('l') || c0 == QLatin1Char('r')) {
        dir = c0 == QLatin1Char('l') ? QStringLiteral("left ")
                                     : QStringLiteral("right ");
        ++i;
    }
    if (i >= tok.size()) return false;
    for (int k = i; k < tok.size(); ++k)
        if (!tok.at(k).isDigit()) return false;
    bool ok = false;
    const int deg = QStringView(tok).mid(i).toInt(&ok);
    // Angles only. A bare "0" or "1" is a variant number, not a heading, and
    // 25 / 15 turn up as speed percentages on the crawl clips — so the test is
    // for values that are actually plausible headings and are multiples of 15.
    if (!ok || deg <= 0 || deg > 360 || deg % 15 != 0) return false;
    *out = dir + sign + QString::number(deg) + QString::fromUtf8("°");
    return true;
}

QString stemOf(const QString& path)
{
    return path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
}

}  // namespace

QString animCategoryName(AnimCategory c)
{
    switch (c) {
    case AnimCategory::Weapon:      return QStringLiteral("Weapon");
    case AnimCategory::ReadyMove:   return QStringLiteral("Ready move");
    case AnimCategory::Locomotion:  return QStringLiteral("Locomotion");
    case AnimCategory::Damage:      return QStringLiteral("Damage");
    case AnimCategory::Cqc:         return QStringLiteral("CQC");
    case AnimCategory::Idle:        return QStringLiteral("Idle");
    case AnimCategory::Facial:      return QStringLiteral("Facial");
    case AnimCategory::Interaction: return QStringLiteral("Interaction");
    default:                        return QStringLiteral("Other");
    }
}

AnimCategory animCategoryFor(const QString& ganiName, const QString& archiveHint)
{
    // The facial archives are stated by the FILE they live in, and by the clip
    // name's own prefix ("enf0face_damage_h") — a facial damage reaction is
    // filed under Facial, not under Damage, because that is the drawer someone
    // goes to for it.
    const QString hint = archiveHint.toLower();
    if (hint.contains(QLatin1String("face")) || hint.contains(QLatin1String("facial"))
        || hint.contains(QLatin1String("deform")))
        return AnimCategory::Facial;

    QString stem = ganiName.section(QLatin1Char('/'), -1);
    const int dot = stem.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) stem.truncate(dot);
    const QStringList parts = stem.toLower().split(QLatin1Char('_'),
                                                   Qt::SkipEmptyParts);
    if (!parts.isEmpty() && parts.first().contains(QLatin1String("face")))
        return AnimCategory::Facial;

    // The SUBJECT token is excluded from the main pass. It names who the clip
    // belongs to ("snapdam", "wkrg", "wolf") and matching inside it would let
    // an archive-wide state override what the clip actually does — every
    // "snaprdy_*" clip would file as Weapon, walk cycles included.
    QSet<QString> toks(parts.begin(), parts.end());
    if (!parts.isEmpty()) toks.remove(parts.first());
    const QVector<QSet<QString>>& sets = ruleSets();
    for (int i = 0; i < sets.size(); ++i)
        for (const QString& t : toks)
            if (sets[i].contains(t)) return kRules[i].cat;

    // LAST RESORT, and only for clips nothing else claimed: the subject token
    // is a subject plus a state glued together — "snap" + "dam", "snap" + "c4"
    // — so its suffix is tested against the same tables. This runs after the
    // main pass precisely so it can never outrank a real action token, and it
    // is what rescues rows like "snapdam_c_down" that carry their meaning in
    // the prefix and nowhere else.
    if (!parts.isEmpty()) {
        const QString subject = parts.first();
        for (int i = 0; i < sets.size(); ++i)
            for (const QString& t : sets[i])
                if (t.size() >= 2 && subject.endsWith(t)) return kRules[i].cat;
    }

    // …and finally the SHELF, which is stated by the path rather than by any
    // token. The weapon-part archives under player2/Magazine, player2/Barrel,
    // player2/Receiver and player2/OtherEquips name their clips after part
    // ids ("snapar01_1_s_am10_3_p") that carry no action at all, so the only
    // thing that says what they are is where they live.
    if (hint.contains(QLatin1String("magazine"))
        || hint.contains(QLatin1String("barrel"))
        || hint.contains(QLatin1String("receiver"))
        || hint.contains(QLatin1String("otherequips"))
        || hint.contains(QLatin1String("equip")))
        return AnimCategory::Weapon;
    return AnimCategory::Other;
}

QString animLabelFor(const QString& ganiName)
{
    QString stem = ganiName.section(QLatin1Char('/'), -1);
    const int dot = stem.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) stem.truncate(dot);
    const QStringList parts = stem.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return stem;

    const QHash<QString, QString>& words = wordMap();
    QStringList out;
    QString stance;
    for (int i = 0; i < parts.size(); ++i) {
        const QString low = parts[i].toLower();
        // The first token is the SUBJECT ("snapnon", "wolf", "wkrg") and is
        // dropped: every clip in an archive shares it, so repeating it on
        // every row is noise the archive's own caption already carries.
        if (i == 0) continue;
        // The stance letter, left as a bare tag. See the header for why: 'c'
        // and 'p' behave like crouch and prone and 's'/'q' do not, which is
        // suggestive and not proof, and a wrong word is worse than a letter.
        if (i == 1 && low.size() == 1
            && (low == QLatin1String("s") || low == QLatin1String("q")
                || low == QLatin1String("c") || low == QLatin1String("p"))) {
            // Held back to the END of the label. It is one unexpanded
            // character and putting it first made every row in the list open
            // with "S ·" — the action, which is what someone is scanning for,
            // has to be the first thing on the line.
            stance = low;
            continue;
        }
        const auto w = words.constFind(low);
        if (w != words.constEnd()) { out << w.value(); continue; }
        QString angle;
        if (expandAngle(low, &angle)) { out << angle; continue; }
        // Fox's transition form: "trot2gllp", "wlk2dsh", "p2upl" — an "a2b"
        // token meaning a to b. Expanded ONLY when BOTH halves are words we
        // already know, so an ordinary token that happens to contain a 2
        // ("blc50", "am02") is left alone.
        const int two = low.indexOf(QLatin1Char('2'));
        if (two > 0 && two < low.size() - 1) {
            const auto a = words.constFind(low.left(two));
            const auto b = words.constFind(low.mid(two + 1));
            if (a != words.constEnd() && b != words.constEnd()) {
                out << a.value() + QString::fromUtf8(" → ") + b.value();
                continue;
            }
        }
        out << parts[i];   // verbatim, original case
    }
    if (out.isEmpty() && stance.isEmpty()) return stem;
    QString label = out.join(QStringLiteral(" · "));
    if (label.isEmpty()) label = stance;
    else if (!stance.isEmpty())
        label += QStringLiteral("  (") + stance + QLatin1Char(')');
    label[0] = label[0].toUpper();
    return label;
}

const AnimCatalog& AnimCatalog::instance()
{
    static AnimCatalog cache;
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

void AnimCatalog::build()
{
    QElapsedTimer t;
    t.start();
    m_archives.clear();
    m_order.clear();
    m_clipCount = 0;
    m_failed = 0;

    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    // The index carries the SAME motion archive several times over: measured
    // here, 275 .mtar entries resolve to 164 distinct paths, with
    // equip/chimera/receiver/hg01_default.mtar appearing six times at an
    // identical size. That is container recursion reaching one file through
    // several .fpk paths, not six different archives — and none is flagged
    // shadowed, because none of them is the copy the game would skip.
    //
    // Folded on path AND size, so two genuinely different files that happen to
    // share a path (a mod installed over stock data) both still appear. The
    // fold count is reported rather than swallowed.
    QSet<QString> seen;
    int folded = 0;
    // ── Phase timing (the 8w play, applied to the next-largest cost) ─────
    // Measured on the user's install: this catalogue is 17790 ms cold and
    // 7058 ms warm, against an index that is 1080 ms warm — so it is now
    // essentially the whole startup and nothing has ever said WHICH part of it
    // costs that. 508 archives are read, decompressed and parsed, and 24,353
    // clips are each run through the category rules and the label builder;
    // those are very different kinds of work and the fix for one is not the
    // fix for the other.
    //
    // Instrument, ship, read the number, THEN decide — the same order that
    // stopped 8w parallelising a discovery walk which turned out to be 274 ms.
    qint64 msRead = 0, msParse = 0, msUnits = 0, msClips = 0;
    QElapsedTimer ph;
    for (int i = 0; i < files.size(); ++i) {
        const IndexedFile& f = files[i];
        if (ArchiveIndex::extensionOf(f) != QLatin1String("mtar")) continue;
        if (f.shadowed) continue;
        if (!GameFilter::instance().enabled(index.gameOf(f))) continue;
        const QString key = f.path + QLatin1Char('|') + QString::number(f.size);
        if (seen.contains(key)) { ++folded; continue; }
        seen.insert(key);

        MtarFile m;
        ph.start();
        const QByteArray data = index.readFile(f);
        msRead += ph.nsecsElapsed();
        if (data.isEmpty()) { ++m_failed; continue; }
        ph.restart();
        const bool parsed = m.parse(data);
        msParse += ph.nsecsElapsed();
        if (!parsed) { ++m_failed; continue; }

        AnimArchive a;
        a.fileIdx = i;
        a.path = f.path;
        a.stem = stemOf(f.path);
        a.v2 = m.isV2();
        // "/Assets/tpp/motion/mtar/player2/Receiver/x.mtar" — the game is the
        // segment after /Assets/, and the shelf is everything between
        // motion/mtar/ and the filename. Both come straight off the path; if
        // the path is not in that shape the fields are simply left empty
        // rather than guessed at from the name.
        const QStringList seg = f.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (seg.size() > 1 && seg.first().compare(QLatin1String("Assets"),
                                                  Qt::CaseInsensitive) == 0)
            a.game = seg.at(1);
        const int cut = f.path.indexOf(QLatin1String("/motion/mtar/"), 0,
                                       Qt::CaseInsensitive);
        if (cut >= 0) {
            const QString tail = f.path.mid(cut + 13);
            if (tail.contains(QLatin1Char('/')))
                a.group = tail.section(QLatin1Char('/'), 0, -2);
        }

        // The archive's skeleton signature, from the shared layout it already
        // parsed. Free for a v2 archive; a v1/GZ archive keeps its layout per
        // clip, so clip 0 is decoded to stand for the set — one decode per
        // archive, not one per clip.
        {
            ph.restart();
            QSet<quint32> hs;
            if (m.layout().valid()) {
                a.unitCount = m.layout().unitCount;
                for (const GaniLayoutUnit& u : m.layout().units)
                    hs.insert(u.nameHash);
            } else if (!m.clips().isEmpty()) {
                const GaniAnim probe = m.decodeClip(0);
                if (probe.valid()) {
                    a.unitsFromClip0 = true;
                    a.unitCount = probe.tracks.size();
                    for (const GaniTrack& t : probe.tracks)
                        hs.insert(t.nameHash);
                }
            }
            a.unitHashes = QVector<quint32>(hs.cbegin(), hs.cend());
            std::sort(a.unitHashes.begin(), a.unitHashes.end());
            msUnits += ph.nsecsElapsed();
        }

        const QString hint = a.stem + QLatin1Char('/') + a.group;
        ph.restart();
        a.clips.reserve(m.clips().size());
        for (int c = 0; c < m.clips().size(); ++c) {
            const MtarClip& mc = m.clips()[c];
            AnimClip clip;
            clip.index = c;
            // An unnamed clip keeps its hash as its name. That happens on a
            // partial install where the dictionary could not resolve the gani
            // path, and showing the hash is more honest than showing nothing.
            clip.name = mc.name.isEmpty()
                ? QStringLiteral("0x%1").arg(mc.hash, 0, 16)
                : stemOf(mc.name);
            clip.category = animCategoryFor(clip.name, hint);
            clip.label = mc.name.isEmpty() ? clip.name : animLabelFor(clip.name);
            a.perCategory[int(clip.category)]++;
            a.clips.append(clip);
            ++m_clipCount;
        }
        msClips += ph.nsecsElapsed();
        m_archives.append(a);
    }

    // Display order: game, then shelf, then name. Sorting the INDICES rather
    // than the archives keeps fileIdx-to-archive lookups stable for anyone who
    // held on to one.
    m_order.resize(m_archives.size());
    for (int i = 0; i < m_order.size(); ++i) m_order[i] = i;
    std::sort(m_order.begin(), m_order.end(), [this](int a, int b) {
        const AnimArchive& x = m_archives[a];
        const AnimArchive& y = m_archives[b];
        if (x.game != y.game) return x.game < y.game;
        if (x.group != y.group) return x.group < y.group;
        return x.stem < y.stem;
    });

    QSet<QString> groups;
    for (const AnimArchive& a : m_archives)
        groups.insert(a.game + QLatin1Char('/') + a.group);
    m_note = QStringLiteral("%1 archive(s), %2 clip(s), %3 group(s)")
                 .arg(m_archives.size())
                 .arg(m_clipCount)
                 .arg(groups.size());
    if (folded)
        m_note += QStringLiteral(", %1 duplicate entr%2 folded")
                      .arg(folded)
                      .arg(folded == 1 ? QStringLiteral("y")
                                       : QStringLiteral("ies"));
    if (m_failed)
        m_note += QStringLiteral(", %1 unreadable").arg(m_failed);
    // Nanoseconds accumulated, reported in ms: 508 archives at a fraction of a
    // millisecond each round to zero if each is timed in ms, which is how a
    // phase that is really seconds can be reported as nothing at all.
    const auto ms = [](qint64 ns) { return static_cast<long long>(ns / 1000000); };
    qInfo("anims: %s in %lld ms — read %lld ms, parse %lld ms, units %lld ms, "
          "clips %lld ms, rest %lld ms",
          qUtf8Printable(m_note), static_cast<long long>(t.elapsed()),
          ms(msRead), ms(msParse), ms(msUnits), ms(msClips),
          static_cast<long long>(t.elapsed())
              - ms(msRead + msParse + msUnits + msClips));
}

int animBindCeiling(const QVector<quint32>& modelBoneHashes, const FrigFile* frig)
{
    if (modelBoneHashes.isEmpty()) return 0;
    if (!frig || !frig->valid()) return modelBoneHashes.size();
    int matched = 0;
    // Every track the rig could ever ask for. resolveBoneDrives drops a drive
    // whose track index is past the count it is given, so an unbounded count
    // is exactly "what this rig can do with a complete clip".
    return frig->resolveBoneDrives(modelBoneHashes, INT_MAX, &matched).size();
}

float animBindScore(const AnimArchive& a, const QVector<quint32>& modelBoneHashes,
                    const FrigFile* frig, int* driven, int ceiling)
{
    if (driven) *driven = 0;
    if (modelBoneHashes.isEmpty() || a.unitCount <= 0) return 0.0f;
    int hit = 0;
    if (frig && frig->valid()) {
        int matched = 0;
        hit = frig->resolveBoneDrives(modelBoneHashes, a.unitCount, &matched)
                  .size();
    } else {
        // No rig: a direct merge of the archive's unit-name hashes against the
        // model's bone-name hashes.
        //
        // MEASURED, AND IT IS ALWAYS ZERO. Against an MGO avatar body (116
        // bones) and a Survive arm (102 bones), all 108 archives in the test
        // tree overlap on exactly 0 names. The reason is a category error this
        // branch has carried from the start and which AnimBind.h's own header
        // states the truth of: "a gani animates RIG UNITS — track i drives
        // unit i". A gani layout unit's nameHash is a RIG UNIT name; an FMDL
        // bone's hash32 is a BONE name. They are different vocabularies, so
        // this merge can only ever return 0 and this branch has never
        // contributed a binding.
        //
        // Left in place rather than deleted because deleting it would change
        // nothing and hide the finding. The real comparison is against the
        // model's .frig — see the note on animBindScore below.
        int i = 0, j = 0;
        while (i < a.unitHashes.size() && j < modelBoneHashes.size()) {
            if (a.unitHashes[i] < modelBoneHashes[j]) ++i;
            else if (modelBoneHashes[j] < a.unitHashes[i]) ++j;
            else { ++hit; ++i; ++j; }
        }
    }
    if (driven) *driven = hit;
    const int ceil =
        ceiling >= 0 ? ceiling : animBindCeiling(modelBoneHashes, frig);
    return ceil > 0 ? qMin(1.0f, float(hit) / float(ceil)) : 0.0f;
}

// ── HOW MANY OF THIS ARCHIVE'S RIG UNITS THE MODEL'S RIG ALSO NAMES ──────
//
// DIAGNOSTIC ONLY. Nothing filters on this yet, and that is deliberate.
//
// The scoring above answers the rigged case with
//     hit = resolveBoneDrives(modelBones, a.unitCount).size()
// over resolveBoneDrives(modelBones, INT_MAX).size(). Both sides depend on the
// archive ONLY through a.unitCount — its TRACK COUNT. So the question it
// actually asks is "does this archive have enough tracks", and every archive
// with enough tracks scores 1.000. On humanoids that is harmless because the
// humanoid archives really are interchangeable; on a Walker Gear it is why
// /Assets/tpp/mecha/mgm came back bound to 23,713 of 24,353 clips.
//
// What SHOULD decide it is the name agreement between the archive's rig units
// and the model's rig. FrigFile::bones() carries that vocabulary
// (FrigBone::nameHash32); the FMDL's bone hashes do not, as the zero overlap
// above proves. This function measures it so ONE run of --animbind on a real
// install settles whether that separates walkergear2 from the rest — the
// container has one .frig in it and cannot answer the question.
int animBindNameOverlap(const AnimArchive& a, const FrigFile* frig)
{
    if (!frig || !frig->valid() || a.unitHashes.isEmpty()) return -1;
    QVector<quint32> rigNames;
    rigNames.reserve(frig->bones().size());
    for (const FrigFile::FrigBone& b : frig->bones())
        rigNames.append(b.nameHash32);
    std::sort(rigNames.begin(), rigNames.end());
    rigNames.erase(std::unique(rigNames.begin(), rigNames.end()), rigNames.end());
    int hit = 0, i = 0, j = 0;
    while (i < a.unitHashes.size() && j < rigNames.size()) {
        if (a.unitHashes[i] < rigNames[j]) ++i;
        else if (rigNames[j] < a.unitHashes[i]) ++j;
        else { ++hit; ++i; ++j; }
    }
    return hit;
}

float animBindThreshold()
{
    // MEASURED, not picked. --animbind prints the whole distribution, and it
    // is bimodal with nothing in the middle. Against three humanoids (a TPP
    // avatar body, an MGO avatar body and the MGO base skeleton) the answer
    // was identical: 72 of 109 archives score exactly 1.000 — every player,
    // character and avatar-deform set — and the other 37 (weapon receivers,
    // Fulton, decoys) top out at 0.358. Nothing at all lands between 0.359
    // and 0.999. Half sits in the middle of that band, so the number does not
    // have to be exact; and because the probe prints the histogram rather
    // than a verdict, an install that narrows the gap shows up as a reading
    // instead of as a filter quietly getting it wrong.
    return 0.5f;
}

QVector<quint32> modelBoneHashes(const FmdlFile& model)
{
    QSet<quint32> hs;
    for (const auto& b : model.bones()) hs.insert(b.nameHash32());
    QVector<quint32> out(hs.cbegin(), hs.cend());
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace fox
