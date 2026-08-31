// ModelTags.cpp — see ModelTags.h.
#include "index/ModelTags.h"

#include <QRegularExpression>

#include "index/ArchiveIndex.h"
#include "index/BuildTimer.h"

namespace fox {
namespace {

// A tag has to survive being typed into a search box, so it may not contain a
// space or a '#'. Everything else is kept as-is: the asset tree's own names are
// already terse and lower-case, and rewriting them would only make the tag
// stop matching what the path says.
QString sanitise(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        // Anything the search box's own grammar would eat becomes a hyphen, so
        // every tag can be typed back in verbatim. ':' matters because a loose
        // mount's name really is "loose:loose".
        if (c.isSpace() || c == QLatin1Char('#') || c == QLatin1Char('"')
            || c == QLatin1Char(':'))
            out.append(QLatin1Char('-'));
        else
            out.append(c.toLower());
    }
    while (out.endsWith(QLatin1Char('-'))) out.chop(1);
    return out;
}

// Order a category's tags by how many models carry them, then alphabetically so
// the order is stable between runs. Frequency first because the popup shows the
// common families at the top where they can be found without reading.
void sortByCount(QVector<TagInfo>& v)
{
    std::sort(v.begin(), v.end(), [](const TagInfo& a, const TagInfo& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.tag < b.tag;
    });
}

}  // namespace

const ModelTags& ModelTags::instance()
{
    static ModelTags c;
    const ArchiveIndex& index = ArchiveIndex::instance();
    static const void* key = nullptr;
    const void* now = index.files().constData();
    // Do NOT cache against an index that is not ready yet: build() would return
    // an empty vocabulary and this key would then hold it there for good.
    if (now != key && index.ready()) {
        key = now;
        c.build();
    }
    return c;
}

void ModelTags::build()
{
    BuildTimer bt("tags");
    m_byFile.clear();
    m_tagCategory.clear();
    m_tagLabel.clear();
    m_categories.clear();

    const ArchiveIndex& index = ArchiveIndex::instance();
    if (!index.ready()) return;
    const auto& files = index.files();
    const auto& archives = index.archives();

    // ONE owner per tag name, and one count per tag name.
    //
    // The categories are not disjoint vocabularies: "chara" is an asset type
    // (/Assets/tpp/chara/...) and also a family (/Assets/tpp/common_source/
    // chara/...). Letting both claim it produced two popup rows with the same
    // name and different counts, and made the OR/AND grouping depend on which
    // one happened to be inserted last. So a tag is claimed by the FIRST
    // category to see it — the loop below runs them in priority order, game
    // through status — and its count is the number of models carrying it from
    // any source, which is what the search box actually returns.
    QHash<QString, QString> tagCat;     // tag → owning category id
    QHash<QString, int> tagCount;       // tag → models carrying it
    QHash<QString, QString> tagLabel;   // tag → display label
    QHash<QString, QString> tagHint;
    // Ownership is decided by CATEGORY PRIORITY, and the priority has to be
    // tracked explicitly rather than left to "whoever claimed it first". The
    // loop below runs files on the outside and facets on the inside, so a
    // first-come rule would let the order files happen to sit in the archive
    // decide whether "chara" is an asset type or a family — and since
    // satisfies() groups by category, that would silently flip #chara #weapon
    // between a union and an (always empty) intersection depending on the
    // install's scan order.
    QHash<QString, int> tagPrio;        // tag → best (lowest) priority seen
    const auto prioOf = [](const QString& cat) {
        if (cat == QLatin1String("game")) return 0;
        if (cat == QLatin1String("category")) return 1;
        if (cat == QLatin1String("family")) return 2;
        if (cat == QLatin1String("variant")) return 3;
        if (cat == QLatin1String("source")) return 4;
        return 5;                        // status
    };
    const auto claim = [&](const QString& tag, const QString& cat,
                           const QString& label, QStringList& into) {
        if (tag.isEmpty()) return;
        const int p = prioOf(cat);
        const auto it = tagPrio.constFind(tag);
        if (it == tagPrio.constEnd() || p < it.value()) {
            tagPrio.insert(tag, p);
            tagCat.insert(tag, cat);
            tagLabel.insert(tag, label);
        }
        // Counted once per FILE, however many facets produced it.
        if (into.contains(tag)) return;
        into.append(tag);
        ++tagCount[tag];
    };

    for (int i = 0; i < files.size(); ++i) {
        const IndexedFile& f = files[i];
        if (ArchiveIndex::extensionOf(f) != QLatin1String("fmdl")) continue;
        QStringList tags;

        // ── game ────────────────────────────────────────────────────────────
        const GameId g = index.gameOf(f);
        claim(sanitise(QString::fromLatin1(gameShortName(g))),
              QStringLiteral("game"), QString::fromLatin1(gameShortName(g)),
              tags);

        // ── category / family, from the asset path ──────────────────────────
        //
        // /Assets/<root>/<category>/<family>/... — the root is the game's own
        // folder (tpp, ssd, mgo) and carries nothing the game tag does not, so
        // it is skipped. An unnamed entry has no path at all and simply gets
        // neither tag, which is correct: nothing is known about where it lives.
        if (f.named && f.path.startsWith(QLatin1String("/Assets/"))) {
            const QVector<QStringView> seg =
                QStringView(f.path).split(QLatin1Char('/'), Qt::SkipEmptyParts);
            // seg[0] = "Assets", seg[1] = root, seg[2] = category, seg[3] = family
            if (seg.size() > 2)
                claim(sanitise(seg[2].toString()), QStringLiteral("category"),
                      seg[2].toString(), tags);
            if (seg.size() > 3) {
                const QString fam = sanitise(seg[3].toString());
                // "Scenes" is where every model in the tree lives; a tag that
                // almost everything carries is not a filter, so the family is
                // only taken when it is a real folder name.
                if (fam != QLatin1String("scenes"))
                    claim(fam, QStringLiteral("family"), seg[3].toString(), tags);
            }
        }

        // ── variant, from the stem's last underscore token ──────────────────
        //
        // Fox stems end in a role: sna0_main0_def, avm_hair_a0_v0_cov. Measured
        // over the indexed models the tail is a small vocabulary (def, cov and
        // a handful more), which makes it a genuinely useful axis. A tail that
        // is all digits is an index, not a role, so it is skipped.
        if (f.named) {
            const QString stem = f.path.section(QLatin1Char('/'), -1)
                                     .section(QLatin1Char('.'), 0, 0);
            const int us = stem.lastIndexOf(QLatin1Char('_'));
            if (us > 0 && us + 1 < stem.size()) {
                const QString tail = sanitise(stem.mid(us + 1));
                bool allDigits = true;
                for (const QChar c : tail)
                    if (!c.isDigit()) { allDigits = false; break; }
                if (!allDigits && tail.size() <= 8)
                    claim(tail, QStringLiteral("variant"), tail, tags);
            }
        }

        // ── source archive ──────────────────────────────────────────────────
        if (f.archiveId >= 0 && f.archiveId < archives.size()) {
            const QString shortName = archives[f.archiveId].shortName;
            claim(sanitise(shortName.section(QLatin1Char('.'), 0, 0)),
                  QStringLiteral("source"), shortName, tags);
        }

        // ── status, from the index's own flags ──────────────────────────────
        claim(f.named ? QStringLiteral("named") : QStringLiteral("hashed"),
              QStringLiteral("status"),
              f.named ? QStringLiteral("named") : QStringLiteral("hashed"), tags);
        if (f.shadowed)
            claim(QStringLiteral("shadowed"), QStringLiteral("status"),
                  QStringLiteral("shadowed"), tags);
        if (f.gz)
            claim(QStringLiteral("gz-archive"), QStringLiteral("status"),
                  QStringLiteral("gz-archive"), tags);
        if (f.archiveId >= 0 && f.archiveId < archives.size()
            && archives[f.archiveId].kind == ArchiveKind::Loose)
            claim(QStringLiteral("loose"), QStringLiteral("status"),
                  QStringLiteral("loose"), tags);

        m_byFile.insert(i, tags);
    }

    if (m_byFile.isEmpty()) return;

    // "hashed" is a control somebody goes looking for. An install that happens
    // to have none should say so with a 0 beside it rather than silently omit
    // the box and leave you wondering whether the feature exists at all, so
    // both halves of the named/hashed pair are forced into the vocabulary.
    for (const char* k : {"named", "hashed"}) {
        const QString t = QLatin1String(k);
        if (!tagCat.contains(t)) {
            tagCat.insert(t, QStringLiteral("status"));
            tagLabel.insert(t, t);
            tagCount.insert(t, 0);
        }
    }

    tagHint.insert(QStringLiteral("named"),
                   QStringLiteral("A dictionary resolves this entry's key to a "
                                  "real asset path."));
    tagHint.insert(
        QStringLiteral("hashed"),
        QStringLiteral("No name in any loaded dictionary resolves this key, so "
                       "the entry shows as a bare hash. The model still opens — "
                       "its extension is stored in the key — it just has no "
                       "path. This is where unnamed content lives."));
    tagHint.insert(QStringLiteral("shadowed"),
                   QStringLiteral("Another archive with higher mount priority "
                                  "carries the same key, so the game would not "
                                  "load this copy."));
    tagHint.insert(QStringLiteral("gz-archive"),
                   QStringLiteral("Came out of a Ground Zeroes .g0s archive, "
                                  "which hashes names differently."));
    tagHint.insert(QStringLiteral("loose"),
                   QStringLiteral("Mounted from a plain folder of extracted "
                                  "assets rather than an archive."));

    m_tagCategory = tagCat;
    m_tagLabel = tagLabel;

    struct CatDef { const char* id; const char* label; const char* hint; };
    static const CatDef kDefs[] = {
        {"game", "Game",
         "Which game the asset belongs to. With no game ticked this list "
         "follows the app-wide game switches; tick one or more and they take "
         "over for this list only, so you can look at a game you have switched "
         "off elsewhere without reconfiguring anything."},
        {"category", "Asset type",
         "The folder under /Assets/<game>/ — what kind of thing this is."},
        {"family", "Family",
         "The folder below that: the specific character, weapon or item family."},
        {"variant", "Variant",
         "The role at the end of the file's own name — sna0_main0_DEF, "
         "avm_hair_a0_v0_COV."},
        {"source", "Source", "The archive the entry came out of."},
        {"status", "Status", "What the index knows about the entry itself."},
    };
    for (const CatDef& d : kDefs) {
        TagCategory cat;
        cat.id = QLatin1String(d.id);
        cat.label = QLatin1String(d.label);
        cat.hint = QLatin1String(d.hint);
        for (auto it = tagCat.constBegin(); it != tagCat.constEnd(); ++it) {
            if (it.value() != cat.id) continue;
            TagInfo t;
            t.tag = it.key();
            t.label = tagLabel.value(it.key(), it.key());
            t.hint = tagHint.value(it.key());
            t.count = tagCount.value(it.key(), 0);
            cat.tags.append(t);
        }
        sortByCount(cat.tags);
        m_categories.append(cat);
    }

    // Only a category with NOTHING in it is dropped. A single-tag category is
    // kept deliberately: "Game — TPP 905" is not a useless checkbox, it is the
    // install telling you it holds one game, and a control that disappears when
    // the answer is boring is a control people think is broken. (An earlier cut
    // dropped anything under two tags and made the Game row vanish entirely on
    // a single-game install, which is exactly that mistake.)
    for (int i = m_categories.size() - 1; i >= 0; --i)
        if (m_categories[i].tags.isEmpty()) m_categories.removeAt(i);
    bt.setNote(QStringLiteral("%1 file(s) tagged, %2 categor(ies)").arg(m_byFile.size()).arg(m_categories.size()));
}

bool ModelTags::satisfies(int fileIdx, const QStringList& must,
                          const QStringList& mustNot) const
{
    if (must.isEmpty() && mustNot.isEmpty()) return true;
    const auto it = m_byFile.constFind(fileIdx);
    const QStringList mine = it == m_byFile.constEnd() ? QStringList() : it.value();

    for (const QString& t : mustNot)
        if (mine.contains(t)) return false;

    if (must.isEmpty()) return true;
    // OR within a category, AND across categories — see the header. A required
    // tag no category defines can never be satisfied, so the query matches
    // nothing, which is what a typo should do.
    QHash<QString, bool> satisfied;
    for (const QString& t : must) {
        const QString cat = m_tagCategory.value(t);
        if (cat.isEmpty()) return false;
        if (!satisfied.contains(cat)) satisfied.insert(cat, false);
        if (mine.contains(t)) satisfied[cat] = true;
    }
    for (auto s = satisfied.constBegin(); s != satisfied.constEnd(); ++s)
        if (!s.value()) return false;
    return true;
}

bool queryMatchesFile(const searchq::Query& q, int fileIdx,
                      const IndexedFile& f)
{
    if (q.isEmpty()) return true;
    if (!ModelTags::instance().satisfies(fileIdx, q.mustTags(), q.mustNotTags()))
        return false;
    if (!q.hasText()) return true;
    // The id form only when the query actually carries one, so an ordinary
    // search pays nothing for it.
    return q.hasIdTerm() ? q.matchesWithId(f.path, f.hash) : q.matches(f.path);
}

}  // namespace fox
