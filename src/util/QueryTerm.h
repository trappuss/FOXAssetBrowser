// QueryTerm.h — the ONE definition of what a single search term matches.
//
// Ported from D4AssetBrowser's `util/QueryTerm.h` (template §4). The rule it
// enforces is the non-negotiable part of the Browse-tab design: every place
// that filters parses through one helper, so a syntax addition lands
// everywhere at once instead of in whichever parser someone remembered.
//
// The bug this prevents is real and has happened in the D4 tool: three
// hand-rolled `hay.contains(term)` matchers drifted, and Bulk Extract silently
// exported a different set from the list the user had filtered.
//
// Syntax handled HERE — the per-TERM part of the language:
//
//   foo         substring, case-insensitive
//   a|b         OR within one term: matches either alternative
//   0x1a2b…     an ID: the entry's own 64-bit path hash, in hex or decimal
//
// Combined with the outer space-AND that `searchq::Query` applies:
// "avm_|avf_ _cov|_def" = (avm_ OR avf_) AND (_cov OR _def). A negated term
// with alternatives excludes when ANY alternative matches — `-a|b` is
// NOT(a OR b), which is what "exclude a or b" means when read aloud.
//
// WHY OR EXISTS: the factory Bulk Extract presets. "All the customization for
// one avatar" is a union of naming families — hair, glasses, hats, the base
// bodies — which an AND-only language cannot express at all.
//
// WHAT IS DIFFERENT FROM D4, and why. D4's ids are SNOs, a small decimal
// number printed all over its UI. Fox has no such number: an entry is
// identified by its 64-bit PathFileNameCode, which is what `--hashdump` prints
// and what an unnamed file is listed under. So the "digits are an id" form
// reads a HASH, in either base, and matches `IndexedFile::hash` — the same
// thing, spelled the way this engine spells it. Collections (`c:`) have no Fox
// equivalent: the tag vocabulary in `index/ModelTags.h` already carries game,
// asset type, family, variant, source and status, and `#tag` reaches all six.
#pragma once
#include <QLatin1Char>
#include <QString>
#include <QStringList>

namespace QueryTerm {

// Is this term an ID rather than a substring? Hex with an 0x prefix, bare hex
// of 8 or more digits, or a run of 6+ decimal digits. The thresholds keep
// "avm0" and "hat21" out: a two-digit number in a file name is a variant, not
// an identity, and treating it as one would make the commonest searches in
// this tool return nothing.
inline bool isIdTerm(const QString& term, quint64* out = nullptr)
{
    QString t = term.trimmed();
    bool hex = false;
    if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        t = t.mid(2);
        hex = true;
    }
    if (t.isEmpty()) return false;
    bool allHexDigits = true, allDecDigits = true;
    for (const QChar c : t) {
        if (!c.isDigit()) allDecDigits = false;
        if (!(c.isDigit() || (c.toLower() >= QLatin1Char('a')
                              && c.toLower() <= QLatin1Char('f'))))
            allHexDigits = false;
    }
    bool ok = false;
    quint64 v = 0;
    if (hex) {
        if (!allHexDigits) return false;
        v = t.toULongLong(&ok, 16);
    } else if (allDecDigits && t.size() >= 6) {
        v = t.toULongLong(&ok, 10);
    } else if (allHexDigits && t.size() >= 8) {
        v = t.toULongLong(&ok, 16);
    } else {
        return false;
    }
    if (!ok) return false;
    if (out) *out = v;
    return true;
}

// The substring/OR test. `hay` is whatever the caller considers searchable —
// in this tool the path, and for a tag-aware caller the path plus its tags.
inline bool matches(const QString& hay, const QString& term)
{
    if (!term.contains(QLatin1Char('|')))
        return hay.contains(term, Qt::CaseInsensitive);
    const QStringList alts = term.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    if (alts.isEmpty()) return true;   // a term that is ONLY pipes constrains nothing
    for (const QString& a : alts)
        if (hay.contains(a, Qt::CaseInsensitive)) return true;
    return false;
}

// The same test, with an id in hand. A term that reads as an id matches the
// entry whose hash it names, and NOTHING else — not the text of the path, which
// would make a hash search return every file whose name happens to contain
// those digits.
inline bool matchesWithId(const QString& hay, quint64 id, const QString& term)
{
    quint64 want = 0;
    if (isIdTerm(term, &want)) return id == want;
    return matches(hay, term);
}

// ── Self-test ───────────────────────────────────────────────────────────────
// Returns "" on success, or the first failing case. Called once at startup
// (main.cpp), which costs microseconds and is the cheapest guard against the
// specific regression this header exists to prevent: someone "simplifying" one
// of the matchers back to a bare contains(). That would not fail to compile
// and would not crash — Bulk Extract would just quietly return a different set
// from the list it was filtered in.
inline QString selfTest()
{
    struct Case { const char* hay; const char* term; bool want; };
    static const Case kCases[] = {
        // plain terms behave exactly as a bare contains() did
        {"avm0_type0_def",        "avm0_type",            true },
        {"avm0_type0_def",        "avf0_type",            false},
        {"avm0_type0_def",        "AVM0_TYPE",            true },   // case-insensitive
        // alternatives
        {"avm0_type0_def",        "avf0|avm0",            true },
        {"avm0_type0_def",        "avf0|ssd0",            false},
        {"hat21_main0_def",       "hat|gls",              true },
        {"tcl0_main0_def_f",      "tcl0_main0_def|rcl0_main0_def", true },
        {"tcl1_main0_def_f",      "tcl0_main0_def|rcl0_main0_def", false},
        // degenerate input must not match everything
        {"anything",              "|",                    true },
        {"anything",              "zzz|",                 false},
    };
    for (const auto& c : kCases) {
        const QString hay = QString::fromLatin1(c.hay);
        const QString term = QString::fromLatin1(c.term);
        if (matches(hay, term) != c.want)
            return QStringLiteral("QueryTerm: \"%1\" vs \"%2\" expected %3")
                .arg(hay, term,
                     c.want ? QStringLiteral("match") : QStringLiteral("no match"));
    }

    // The id form. A hash term must match ONLY the entry carrying that hash,
    // and a short number must stay a substring — "hat21" and "avm0" are the
    // commonest things anyone types into this tool.
    struct IdCase { const char* term; bool isId; quint64 value; };
    static const IdCase kIds[] = {
        {"0x1a2b3c4d5e6f7a8b", true,  0x1a2b3c4d5e6f7a8bULL},
        {"0X1A2B",             true,  0x1a2bULL},
        {"1234567890",         true,  1234567890ULL},
        {"deadbeef",           true,  0xdeadbeefULL},
        {"hat21",              false, 0},
        {"avm0",               false, 0},
        {"12345",              false, 0},   // five digits: a variant, not an id
        {"def",                false, 0},   // three hex letters, but too short
        {"",                   false, 0},
        {"0x",                 false, 0},
    };
    for (const auto& c : kIds) {
        quint64 got = 0;
        const QString term = QString::fromLatin1(c.term);
        const bool is = isIdTerm(term, &got);
        if (is != c.isId)
            return QStringLiteral("QueryTerm: \"%1\" id-ness expected %2")
                .arg(term, c.isId ? QStringLiteral("true") : QStringLiteral("false"));
        if (is && got != c.value)
            return QStringLiteral("QueryTerm: \"%1\" parsed as %2, expected %3")
                .arg(term)
                .arg(got)
                .arg(c.value);
    }
    // …and the combination: an id term must NOT fall back to text matching.
    if (matchesWithId(QStringLiteral("/Assets/1234567890_thing.fmdl"), 42,
                      QStringLiteral("1234567890")))
        return QStringLiteral(
            "QueryTerm: an id term matched a path containing those digits");
    if (!matchesWithId(QStringLiteral("/Assets/anything.fmdl"), 1234567890ULL,
                       QStringLiteral("1234567890")))
        return QStringLiteral("QueryTerm: an id term did not match its own hash");
    return QString();
}

}  // namespace QueryTerm
