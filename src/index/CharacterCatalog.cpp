// CharacterCatalog.cpp — see CharacterCatalog.h.
#include "index/CharacterCatalog.h"

#include <QRegularExpression>
#include <algorithm>

#include "index/ArchiveIndex.h"
#include "index/BuildTimer.h"
#include "index/GameId.h"

namespace fox {
namespace {

// Slot order as a person would build a character.
const char* const kSlotOrder[] = {
    "body", "face", "head", "hair", "eyebrow", "beard", "tattoo", "arm", "gear",
};

QString stemOf(const QString& path)
{
    return path.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
}

// Character id → a readable group name where the code is unambiguous in the
// asset tree. Unknown codes keep their raw id rather than being given a name.
QString groupNameFor(const QString& dir)
{
    static const QHash<QString, QString> kNames = {
        {QStringLiteral("sna"), QStringLiteral("Snake")},
        {QStringLiteral("dds"), QStringLiteral("Diamond Dogs soldier")},
        {QStringLiteral("avm"), QStringLiteral("Avatar")},
        {QStringLiteral("qui"), QStringLiteral("Quiet")},
        {QStringLiteral("oce"), QStringLiteral("Ocelot")},
        {QStringLiteral("kaz"), QStringLiteral("Miller")},
        {QStringLiteral("ish"), QStringLiteral("Ishmael")},
        {QStringLiteral("hrs"), QStringLiteral("D-Horse")},
        {QStringLiteral("ddg"), QStringLiteral("D-Dog")},
        {QStringLiteral("nin"), QStringLiteral("Cyborg Ninja")},
        {QStringLiteral("rai"), QStringLiteral("Raiden")},
    };
    return kNames.value(dir, dir);
}

// A subject is a DIRECTORY ("sna"), not a model prefix ("sna5"), so a part
// belongs to it when its prefix starts with that directory code.
const CatalogPart* ownPartIn(const QHash<QString, QVector<CatalogPart>>& bySlot,
                             const QString& subjectId, const QString& slot)
{
    const auto it = bySlot.constFind(slot);
    if (it == bySlot.constEnd()) return nullptr;
    for (const CatalogPart& p : it.value())
        if (CharacterCatalog::characterOf(p.displayName).startsWith(subjectId))
            return &p;
    return nullptr;
}

}  // namespace

QString CharacterCatalog::fovaKey(const QString& stem)
{
    static const char* const kTails[] = {"_def", "_ply", "_cov", "_sta"};
    QString out = stem;
    // The avatar's skin tones are authored as avm0_main0_skin0_cNN against the
    // model avm0_main0_def, so the "_skinN" marker has to come off too.
    static const QRegularExpression skinRe{QStringLiteral("_skin\\d+$")};
    const QRegularExpressionMatch sm = skinRe.match(out);
    if (sm.hasMatch()) out.chop(sm.capturedLength());
    for (const char* t : kTails) {
        const QString tail = QLatin1String(t);
        if (out.endsWith(tail)) return out.left(out.size() - tail.size());
    }
    return out;
}

QString CharacterCatalog::characterOf(const QString& stem)
{
    // "sna5_main0_def" → "sna5", "avm_hair_a0_v0_cov" → "avm". The trailing
    // digits are OPTIONAL: the avatar's 165 hairstyles, its beards and its
    // eyebrows are all named with a bare three-letter code, and requiring a
    // digit dropped every one of them before they reached a slot.
    const QString head = stem.section(QLatin1Char('_'), 0, 0);
    if (head.size() < 3) return {};
    for (int i = 0; i < 3; ++i)
        if (!head[i].isLetter()) return {};
    for (int i = 3; i < head.size(); ++i)
        if (!head[i].isDigit()) return {};
    return head;
}

QString CharacterCatalog::kindOf(const QString& stem)
{
    const QString second = stem.section(QLatin1Char('_'), 1, 1);
    if (second.isEmpty()) return {};
    int i = 0;
    while (i < second.size() && second[i].isLetter()) ++i;
    return i > 0 ? second.left(i) : QString();
}

QString CharacterCatalog::slotForKind(const QString& kind)
{
    if (kind == QLatin1String("main") || kind == QLatin1String("body")
        || kind == QLatin1String("plym") || kind == QLatin1String("plyf"))
        return QStringLiteral("body");
    if (kind == QLatin1String("face")) return QStringLiteral("face");
    if (kind == QLatin1String("hair")) return QStringLiteral("hair");
    // The avatar editor's own axes. "type" is the face/head shape the game
    // picks from (avm0_type0, avm0_type1 …), and the rest are the pieces laid
    // over it: 165 hairstyles for the male avatar alone, plus beards, eyebrows
    // and tattoos. Without these the whole avatar wardrobe was dropped on the
    // floor, because an unmapped kind is not shown.
    if (kind == QLatin1String("type") || kind == QLatin1String("hone"))
        return QStringLiteral("head");
    if (kind == QLatin1String("berd")) return QStringLiteral("beard");
    if (kind == QLatin1String("ebrw")) return QStringLiteral("eyebrow");
    if (kind == QLatin1String("tato")) return QStringLiteral("tattoo");
    if (kind == QLatin1String("base")) return QStringLiteral("body");
    if (kind == QLatin1String("head") || kind == QLatin1String("hood"))
        return QStringLiteral("head");
    if (kind == QLatin1String("arm") || kind == QLatin1String("rkt")
        || kind == QLatin1String("hand"))
        return QStringLiteral("arm");
    if (kind == QLatin1String("eqhd") || kind == QLatin1String("ephd"))
        return QStringLiteral("head");
    if (kind == QLatin1String("eqit") || kind == QLatin1String("eqbd")
        || kind == QLatin1String("coat"))
        return QStringLiteral("gear");
    return {};
}

const CharacterCatalog& CharacterCatalog::instance()
{
    static CharacterCatalog cache;
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

void CharacterCatalog::build()
{
    BuildTimer bt("chara");
    m_slotNames.clear();
    m_bySlot.clear();
    m_variations.clear();
    m_subjects.clear();

    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    static const QRegularExpression modelRe(
        QStringLiteral("/Assets/[^/]+/(chara|item)/([a-z]{3})/Scenes/([^/]+)\\.fmdl$"),
        QRegularExpression::CaseInsensitiveOption);

    QHash<QString, QString> idToDir;
    for (int i = 0; i < files.size(); ++i) {
        const IndexedFile& f = files[i];
        if (!f.named || f.shadowed) continue;
        // A game the toggles have turned off contributes nothing, anywhere.
        if (!GameFilter::instance().enabled(index.gameOf(f))) continue;
        if (f.path.endsWith(QLatin1String(".fv2"), Qt::CaseInsensitive)) {
            const QString stem = stemOf(f.path);
            const int cut = stem.lastIndexOf(QLatin1Char('_'));
            if (cut <= 0) continue;
            CatalogVariation v;
            v.name = stem.mid(cut + 1);
            v.path = f.path;
            v.fileIdx = i;
            m_variations[fovaKey(stem.left(cut))].append(v);
            continue;
        }
        const QRegularExpressionMatch m = modelRe.match(f.path);
        if (!m.hasMatch()) continue;
        const QString stem = m.captured(3);
        // "_patched" duplicates and LOD stand-ins are not selectable parts.
        if (stem.endsWith(QLatin1String("_patched"))) continue;
        const QString slot = slotForKind(kindOf(stem));
        if (slot.isEmpty()) continue;
        const QString cid = characterOf(stem);
        if (cid.isEmpty()) continue;
        idToDir.insert(cid, m.captured(2));
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

    // Subjects = DIRECTORIES that have at least one body model, not model
    // prefixes. The game presents one Snake with a wardrobe, not ten Snakes:
    // sna0…sna9 are his uniforms (sna5 is BATTLE DRESS, sna4 the SNEAKING
    // SUIT), and dds3/5/6/8 are one Diamond Dogs soldier in different kit. So
    // the uniform name belongs on the VARIANT, where displayNameFor() puts it,
    // and the subject is the person.
    QHash<QString, CatalogSubject> byId;
    for (const CatalogPart& b : m_bySlot.value(QStringLiteral("body"))) {
        const QString cid = characterOf(b.displayName);
        if (cid.isEmpty()) continue;
        const QString dir = idToDir.value(cid, cid).left(3);
        CatalogSubject& s = byId[dir];
        s.id = dir;
        s.variants.append(b);
    }
    for (auto it = byId.begin(); it != byId.end(); ++it) {
        it->groupName = groupNameFor(it->id);
        for (const QString& slot : m_slotNames) {
            if (slot == QLatin1String("body")) continue;
            if (ownPartIn(m_bySlot, it->id, slot)) ++it->ownPartCount;
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

BuilderSource CharacterCatalog::builderSource() const
{
    BuilderSource src;
    src.subjectLabel = QStringLiteral("Character");
    src.variantLabel = QStringLiteral("Body");
    src.emptyHint = QStringLiteral(
        "No character models found in the configured game folders.");
    src.slotNames = m_slotNames;
    src.subjects = m_subjects;
    src.partsFor = [this](const QString& s) { return m_bySlot.value(s); };
    src.ownPartFor = [this](const QString& id, const QString& s) {
        return ownPartIn(m_bySlot, id, s);
    };
    src.variationsFor = [this](const QString& stem) {
        return m_variations.value(fovaKey(stem));
    };
    // Characters seat gear on bones through the rig, not on weapon-style
    // connect points, so nothing is auto-attached: parts share the skeleton and
    // the animation drives them together, which is what the composer already
    // did before this category existed.
    // Characters share one skeleton — parts are posed by the rig, not seated.
    src.attachPlanFor = [](const QString&) { return QVector<AttachOption>(); };
    return src;
}

}  // namespace fox
