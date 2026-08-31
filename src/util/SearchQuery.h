// SearchQuery.h — the search box's little language, shared by every tab that
// has one so that "-mgo" means the same thing in all of them.
//
//   foo bar     every term must appear   (AND, which is what it always did)
//   +foo        the same, said explicitly
//   -foo        must NOT appear
//   "foo bar"   one term containing a space
//   #tag        require a TAG rather than a substring
//   -#tag       exclude a tag
//   a|b         OR *within* one term — see util/QueryTerm.h
//   0x1a2b…     an entry's own 64-bit path hash, in hex or decimal
//
// The per-term test lives in util/QueryTerm.h and NOT here, because template
// §4 requires exactly one of it in the application: this class owns the
// space-AND, the signs, the quotes and the tags; QueryTerm owns what a single
// term matches, and carries the startup self-test that keeps the two honest.
//
// A tag is not a substring: "#chara" means the file is classified in the chara
// category, where "chara" would also match /Assets/tpp/common_source/chara-ish
// paths. Tags are matched by whoever owns the vocabulary (the Models tab hands
// them to ModelTags), which is why they are kept in their own lists here rather
// than folded in with the text terms. Their AND/OR semantics belong to that
// vocabulary too — see ModelTags.h.
//
// A lone "+" or "-" is text, not an operator: someone searching for a file
// called "a-b" types "a-b" and means it. Only a sign at the START of a term,
// with something after it, is read as an operator.
//
// Matching is case-insensitive substring, which is what all three tabs did
// before and what people expect of a path filter. A query with no positive
// terms matches everything except what it excludes, so "-mgo" alone is a
// useful thing to type.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

#include "util/QueryTerm.h"

namespace searchq {

class Query {
public:
    Query() = default;
    explicit Query(const QString& text) { parse(text); }

    void parse(const QString& text)
    {
        m_must.clear();
        m_mustNot.clear();
        m_mustLiteral.clear();
        m_mustNotLiteral.clear();
        m_mustTags.clear();
        m_mustNotTags.clear();
        for (const Term& raw : split(text)) {
            QString t = raw.text;
            bool negate = false;
            // A QUOTED term is literal, sign and all — that is the only way to
            // search for something that really begins with a hyphen, and
            // without it "-mgo" and -mgo would mean the same thing.
            if (!raw.quoted && t.size() > 1
                && (t.startsWith(QLatin1Char('-'))
                    || t.startsWith(QLatin1Char('+')))) {
                negate = t.startsWith(QLatin1Char('-'));
                t.remove(0, 1);
            }
            // '#' AFTER the sign, so "-#tag" reads as "not this tag" rather
            // than as a term beginning with '#'. A bare '#' is text.
            if (!raw.quoted && t.size() > 1 && t.startsWith(QLatin1Char('#'))) {
                t.remove(0, 1);
                (negate ? m_mustNotTags : m_mustTags).append(t.toLower());
                continue;
            }
            if (t.isEmpty()) continue;
            // A QUOTED term stays literal all the way to the matcher, not just
            // past the sign and the '#'. Without this the escape hatch this
            // class documents stopped working the moment QueryTerm learned two
            // more syntaxes: "a|b" came back OR-ed and "1234567890" came back
            // as an id, so there was no way left to search for a literal pipe
            // or for a long run of digits as text.
            (negate ? m_mustNot : m_must).append(t);
            (negate ? m_mustNotLiteral : m_mustLiteral).append(raw.quoted);
        }
    }

    bool isEmpty() const
    {
        return m_must.isEmpty() && m_mustNot.isEmpty() && m_mustTags.isEmpty()
            && m_mustNotTags.isEmpty();
    }
    // Text terms only — a caller that also has tags tests those separately,
    // because only it knows what a tag means.
    bool hasText() const { return !m_must.isEmpty() || !m_mustNot.isEmpty(); }

    // EVERY per-term test goes through QueryTerm — the one-matcher rule of
    // template §4. It is what makes "a|b" mean the same thing in the Models
    // list, the Files search, the Textures list and Bulk Extract, and it
    // carries a startup self-test so a future "simplification" back to a bare
    // contains() fails loudly instead of quietly returning a different set
    // from the one the user filtered.
    bool matches(const QString& s) const
    {
        for (int i = 0; i < m_mustNot.size(); ++i)
            if (testTerm(s, 0, false, m_mustNot[i], literalNot(i))) return false;
        for (int i = 0; i < m_must.size(); ++i)
            if (!testTerm(s, 0, false, m_must[i], literal(i))) return false;
        return true;
    }

    // The same, for a caller that also knows the entry's own id. A term that
    // reads as an ID (a 64-bit path hash, in hex or decimal — see
    // QueryTerm::isIdTerm) matches that hash and nothing else; every other
    // term is the ordinary substring test. A caller with no id passes 0 and
    // gets exactly the behaviour above.
    bool matchesWithId(const QString& s, quint64 id) const
    {
        for (int i = 0; i < m_mustNot.size(); ++i)
            if (testTerm(s, id, true, m_mustNot[i], literalNot(i))) return false;
        for (int i = 0; i < m_must.size(); ++i)
            if (!testTerm(s, id, true, m_must[i], literal(i))) return false;
        return true;
    }

    // Whether any term in this query is an id. Lets a list decide once,
    // outside its loop, whether it needs the id-aware path at all.
    bool hasIdTerm() const
    {
        for (int i = 0; i < m_must.size(); ++i)
            if (!literal(i) && QueryTerm::isIdTerm(m_must[i])) return true;
        for (int i = 0; i < m_mustNot.size(); ++i)
            if (!literalNot(i) && QueryTerm::isIdTerm(m_mustNot[i])) return true;
        return false;
    }

    const QStringList& must() const { return m_must; }
    const QStringList& mustNot() const { return m_mustNot; }
    const QStringList& mustTags() const { return m_mustTags; }
    const QStringList& mustNotTags() const { return m_mustNotTags; }

    // What a tag is doing in a query. Off is "not mentioned"; the popup's
    // checkboxes cycle through all three, because a two-state box could not
    // express "-#tag" and the syntax would have had a capability the UI did not.
    enum class TagState { Off, Include, Exclude };

    TagState stateOf(const QString& tag) const
    {
        const QString t = tag.toLower();
        if (m_mustTags.contains(t)) return TagState::Include;
        if (m_mustNotTags.contains(t)) return TagState::Exclude;
        return TagState::Off;
    }

    // Rewrite `text` so `tag` is in the given state, leaving everything else in
    // place and untouched. This is what lets a checkbox and the search box be
    // ONE piece of state instead of two that have to be kept agreeing.
    static QString withTag(const QString& text, const QString& tag, TagState st)
    {
        QStringList kept;
        const QString want = QStringLiteral("#") + tag.toLower();
        for (const Term& raw : split(text)) {
            QString t = raw.text;
            QString bare = t;
            if (!raw.quoted && bare.size() > 1
                && (bare.startsWith(QLatin1Char('-'))
                    || bare.startsWith(QLatin1Char('+'))))
                bare.remove(0, 1);
            if (!raw.quoted && bare.compare(want, Qt::CaseInsensitive) == 0)
                continue;   // drop any existing copy, negated or not
            // Put back EXACTLY what was typed. Re-quoting from the stripped
            // text silently rewrote the user's own words — a stray quote in
            // "foo\"bar baz" came back as one term with the quote deleted,
            // and a tag click is the last moment anyone expects their search
            // box to be edited for them.
            kept << raw.raw;
        }
        if (st == TagState::Include) kept << want;
        else if (st == TagState::Exclude) kept << (QLatin1Char('-') + want);
        return kept.join(QLatin1Char(' '));
    }

    // Remove ONE term from a query string, exactly as the chip's ✕ means it.
    // `term` is the term as the chip reports it — "#chara", "-#mgo3", "venom",
    // "-lod" — sign and '#' included, because that is what distinguishes
    // "require this tag" from "exclude it" and the two are different chips.
    //
    // Everything else is put back EXACTLY as typed, for the same reason
    // withTag() does: a chip's ✕ is the last moment anyone expects the rest of
    // their search box to be rewritten for them.
    static QString withoutTerm(const QString& text, const QString& term)
    {
        const QString want = term.trimmed();
        if (want.isEmpty()) return text;
        QStringList kept;
        bool dropped = false;
        for (const Term& raw : split(text)) {
            // Compare on the RAW text, not the parsed remains: only an
            // unquoted term can be the one a chip stands for, and a quoted
            // "-lod" is a literal string the user asked for.
            if (!dropped && !raw.quoted
                && raw.text.compare(want, Qt::CaseInsensitive) == 0) {
                dropped = true;   // one chip, one term — duplicates each keep
                continue;         // their own chip and their own ✕
            }
            kept << raw.raw;
        }
        return kept.join(QLatin1Char(' '));
    }

private:
    bool literal(int i) const
    {
        return i < m_mustLiteral.size() && m_mustLiteral[i];
    }
    bool literalNot(int i) const
    {
        return i < m_mustNotLiteral.size() && m_mustNotLiteral[i];
    }
    // One term against one haystack. A literal term is a plain substring and
    // nothing else; everything else goes through the shared matcher.
    // `haveId` is explicit rather than "id != 0": a hash of zero is a value,
    // not an absence, and testing the value would have made one entry in the
    // index silently fall back to substring matching.
    static bool testTerm(const QString& s, quint64 id, bool haveId,
                         const QString& t, bool isLiteral)
    {
        if (isLiteral) return s.contains(t, Qt::CaseInsensitive);
        return haveId ? QueryTerm::matchesWithId(s, id, t)
                      : QueryTerm::matches(s, t);
    }

    struct Term {
        QString text;    // the term with its quotes stripped, for matching
        QString raw;     // EXACTLY as the user typed it, for rewriting
        bool quoted = false;
    };

    // Space-separated, except inside double quotes. Whether a term WAS quoted
    // travels with it, because that decides whether a leading sign is an
    // operator or part of what is being searched for. An unterminated quote
    // simply runs to the end of the input.
    static QVector<Term> split(const QString& text)
    {
        QVector<Term> out;
        Term cur;
        bool inQuote = false;
        for (const QChar c : text) {
            if (c == QLatin1Char('"')) {
                inQuote = !inQuote;
                // Only a quote that OPENS the term makes it a quoted term. A
                // quote in the middle of a word (foo"bar) is just a character
                // someone typed: treating it as a quoted term used to strip it
                // out and disarm a leading '-', so -ab"c stopped negating.
                if (cur.text.isEmpty() && cur.raw.isEmpty()) cur.quoted = true;
                cur.raw.append(c);
                continue;
            }
            if (!inQuote && c.isSpace()) {
                if (!cur.raw.isEmpty()) out.append(cur);
                cur = Term();
                continue;
            }
            cur.text.append(c);
            cur.raw.append(c);
        }
        if (!cur.raw.isEmpty()) out.append(cur);
        return out;
    }

    QStringList m_must, m_mustNot;
    // Parallel to the two lists above: was that term written in quotes. A
    // quoted term is a plain substring, sign and syntax and all.
    QVector<bool> m_mustLiteral, m_mustNotLiteral;
    QStringList m_mustTags, m_mustNotTags;
};

// The one-line hint every search box shows, so the syntax is discoverable
// rather than something you have to be told.
inline QString tooltip()
{
    return QStringLiteral(
        "Type words to narrow the list — every word must appear.\n"
        "  -word      exclude anything containing it\n"
        "  +word      require it (the same as typing it plain)\n"
        "  \"two words\"  one term with a space in it\n"
        "  #tag       require a tag   (-#tag excludes it)\n"
        "\"-mgo\" on its own shows everything that is not MGO.");
}

// ── The query-string editors' self-test (template §4) ───────────────────────
// QueryTerm::selfTest guards what one TERM matches. This guards what the two
// functions that REWRITE a query string do — withTag, which the funnel popup
// calls, and withoutTerm, which a chip's ✕ calls. They edit the one place
// filter state lives, so a bug in either silently changes the set the user
// filtered without changing anything they can see.
//
// Called once at startup beside the other two, for microseconds.
inline QString selfTest()
{
    struct Case { const char* in; const char* term; const char* want; };
    static const Case kDrop[] = {
        // the ordinary cases
        {"#chara #mgo3",        "#chara",  "#mgo3"},
        {"#chara #mgo3",        "#mgo3",   "#chara"},
        {"venom -lod #chara",   "-lod",    "venom #chara"},
        {"venom -lod #chara",   "venom",   "-lod #chara"},
        {"-#mgo3 venom",        "-#mgo3",  "venom"},
        // a require and an exclude of the same tag are DIFFERENT terms, and
        // dropping one must not take the other
        {"#mgo3 -#mgo3",        "#mgo3",   "-#mgo3"},
        {"#mgo3 -#mgo3",        "-#mgo3",  "#mgo3"},
        // a term that is not there changes nothing
        {"venom #chara",        "#tpp",    "venom #chara"},
        // a QUOTED term is a literal string, not the operator it looks like
        {"\"-lod\" venom",      "-lod",    "\"-lod\" venom"},
        // duplicates each keep their own chip, so one ✕ drops one of them
        {"venom venom",         "venom",   "venom"},
        // dropping the last term leaves an empty query, not a stray space
        {"#chara",              "#chara",  ""},
    };
    for (const auto& c : kDrop) {
        const QString got = Query::withoutTerm(QString::fromUtf8(c.in),
                                               QString::fromUtf8(c.term));
        if (got != QString::fromUtf8(c.want))
            return QStringLiteral("searchq::withoutTerm(\"%1\", \"%2\") = "
                                  "\"%3\", expected \"%4\"")
                .arg(QString::fromUtf8(c.in), QString::fromUtf8(c.term), got,
                     QString::fromUtf8(c.want));
    }

    // withTag's three states, and the round trip a chip depends on: setting a
    // tag then dropping its chip must give back what you started with.
    const QString base = QStringLiteral("venom -lod");
    const QString withInc =
        Query::withTag(base, QStringLiteral("chara"), Query::TagState::Include);
    if (!Query(withInc).mustTags().contains(QStringLiteral("chara")))
        return QStringLiteral("searchq: withTag(Include) did not require it");
    if (Query::withoutTerm(withInc, QStringLiteral("#chara")) != base)
        return QStringLiteral("searchq: include-then-drop did not round trip");
    const QString withExc =
        Query::withTag(base, QStringLiteral("chara"), Query::TagState::Exclude);
    if (!Query(withExc).mustNotTags().contains(QStringLiteral("chara")))
        return QStringLiteral("searchq: withTag(Exclude) did not exclude it");
    if (Query::withoutTerm(withExc, QStringLiteral("-#chara")) != base)
        return QStringLiteral("searchq: exclude-then-drop did not round trip");
    if (Query(Query::withTag(withInc, QStringLiteral("chara"),
                             Query::TagState::Off))
            .mustTags()
            .contains(QStringLiteral("chara")))
        return QStringLiteral("searchq: withTag(Off) left the tag in place");
    return QString();
}

}  // namespace searchq
