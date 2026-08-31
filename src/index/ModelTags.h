// ModelTags.h — the Models tab's tag vocabulary, derived from the index.
//
// Every tag here is MEASURED off the indexed files, never a hard-coded list of
// things the games are assumed to contain: the categories are computed from the
// paths and the index's own per-file facts, so an install carrying assets this
// code has never seen still gets tags for them, and an install missing a whole
// family simply has no tag for it rather than an empty checkbox.
//
// Six categories, and what each is read from:
//
//   game      ArchiveIndex::gameOf()                      tpp, mgo3, gz, survive
//   category  the path segment after /Assets/<root>/      chara, weapon, item…
//   family    the segment after that                      sna, avm, hag, msk…
//   variant   the last underscore token of the stem       def, cov, sta…
//   source    the archive the entry came out of           chunk0, wp_chunk1…
//   status    the index's own flags                       named, hashed,
//                                                         shadowed, gz-archive,
//                                                         loose
//
// A tag name can be produced by more than one facet — "chara" is both an asset
// type and a family — so each name is owned by the HIGHEST-priority category
// that produces it, in the order listed above, and its count is the number of
// models carrying it from any facet.
//
// "hashed" is the interesting one: an entry whose 64-bit key no name in any
// dictionary resolves. Its extension is still known (it is stored in the key),
// so the browser lists it as a model and can open it — it just has no path.
// Filtering to those is how you go looking for what nobody has named yet.
//
// TAG SEMANTICS, which are the conventional faceted ones and not what a bare
// search box does: tags in the SAME category are OR-ed (tpp or mgo3), tags in
// DIFFERENT categories are AND-ed (tpp, and a weapon), and a negated tag always
// excludes. That is what makes the game row work as a set of toggles when it is
// expressed as tags — checking two games has to widen the list, not empty it.
#pragma once
#include <QHash>
#include "index/ArchiveIndex.h"
#include "util/SearchQuery.h"
#include <QString>
#include <QStringList>
#include <QVector>

namespace fox {

struct TagInfo {
    QString tag;     // what you type after '#', always lower-case and unspaced
    QString label;   // what the popup shows — "TPP" reads better than "tpp"
    QString hint;    // one line for the tooltip, or empty
    int count = 0;   // how many models carry it, for the popup's counts
};

struct TagCategory {
    QString id;      // "game"
    QString label;   // "Game"
    QString hint;    // what this category means, for the popup header tooltip
    QVector<TagInfo> tags;   // ordered: see build() for each category's rule
};

class ModelTags {
public:
    // Rebuilt automatically when the archive index changes, like the other
    // catalogues — the files vector's address is the invalidation key.
    static const ModelTags& instance();

    bool ok() const { return !m_byFile.isEmpty(); }
    const QVector<TagCategory>& categories() const { return m_categories; }
    // How many models the vocabulary was built from.
    int modelCount() const { return m_byFile.size(); }

    // The category id a tag belongs to, or empty when nothing defines it. An
    // unknown tag makes a query match nothing, which is the honest answer —
    // silently ignoring it would quietly widen the result instead.
    QString categoryOf(const QString& tag) const
    {
        return m_tagCategory.value(tag);
    }
    bool isTag(const QString& tag) const { return m_tagCategory.contains(tag); }

    // Every tag this model carries (empty for a file that is not a model).
    QStringList tagsOf(int fileIdx) const { return m_byFile.value(fileIdx); }

    // The faceted test described at the top of this file.
    bool satisfies(int fileIdx, const QStringList& must,
                   const QStringList& mustNot) const;

    // Does `tags` name at least one tag from `categoryId`? The Models tab asks
    // this about "game": a game tag typed into the search box has to take over
    // from the app-wide game switches for that list, or asking for a game you
    // had switched off would return nothing and look broken.
    bool categoryHasAny(const QStringList& tags, const QString& categoryId) const
    {
        for (const QString& t : tags)
            if (m_tagCategory.value(t) == categoryId) return true;
        return false;
    }

    // Human-readable label for one tag, for chips and tooltips.
    QString labelFor(const QString& tag) const { return m_tagLabel.value(tag, tag); }

private:
    ModelTags() = default;
    void build();

    QHash<int, QStringList> m_byFile;      // fileIdx → its tags
    QHash<QString, QString> m_tagCategory; // tag → category id
    QHash<QString, QString> m_tagLabel;    // tag → display label
    QVector<TagCategory> m_categories;
};

// ── The ONE test a filtering site applies (template §4) ─────────────────────
// Text terms, id terms and TAG terms, in one call, so the four lists that
// filter the index cannot answer the same query three different ways.
//
// The tag half is why this exists. `searchq::Query` peels "#tag" off into its
// own list and its matches() knows nothing about tags — correctly, because
// only the owner of the vocabulary can say what one means. A site that called
// matches() alone therefore treated "#chara" as a query with NO text terms,
// and a query with no text terms matches everything. In the Bulk Extract tab
// that is not a wrong list, it is a button that writes the entire install to
// disk.
//
// A file the tag map has never seen — anything that is not a model — carries
// no tags, so a required tag excludes it. That is the right answer: "#chara"
// asks for things classified as characters, and an .ftex is not one.
bool queryMatchesFile(const searchq::Query& q, int fileIdx,
                      const IndexedFile& f);

}  // namespace fox
