// MgoGearConfig.cpp — see MgoGearConfig.h.
#include "index/MgoGearConfig.h"

#include <cctype>

#include <QElapsedTimer>
#include <QFile>
#include <QSet>
#include <QRegularExpression>

#include "index/ArchiveIndex.h"

namespace fox {
namespace {

// The file is one enormous line with no whitespace, so this is a brace scanner
// rather than a line parser. Returns the [start,end] of the balanced block
// whose opening brace is at or after `from`; end is the index of the '}'.
bool balanced(const QByteArray& s, int from, int* openAt, int* closeAt)
{
    const int j = s.indexOf('{', from);
    if (j < 0) return false;
    int depth = 0;
    for (int k = j; k < s.size(); ++k) {
        if (s[k] == '{') ++depth;
        else if (s[k] == '}') {
            if (--depth == 0) { *openAt = j; *closeAt = k; return true; }
        }
    }
    return false;
}

// A quoted string field: Name="value". Returns empty when absent in `rec`.
// Word-boundary anchored for the same reason listField is: "ID=\"" is a
// substring of "PurchaseID=\"", and which one indexOf found depended on
// field order inside the record.
// An asset path with its extension removed, which is the form every icon
// consumer wants: IconCatalog::swatchForPath appends ".ftex" itself, so a path
// that still carries one asks the index for "….ftex.ftex" and silently finds
// nothing. GearConfig writes Swatch with the extension; this is where it comes
// off, once, for both the gear icons and the colour icons.
QString assetPathNoExt(const QString& p)
{
    const int slash = p.lastIndexOf(QLatin1Char('/'));
    const int dot = p.indexOf(QLatin1Char('.'), slash + 1);
    return dot < 0 ? p : p.left(dot);
}

QString field(const QByteArray& rec, const char* key)
{
    const QByteArray needle = QByteArray(key) + "=\"";
    int at = rec.indexOf(needle);
    while (at > 0 && (isalnum(uchar(rec[at - 1])) || rec[at - 1] == '_'))
        at = rec.indexOf(needle, at + 1);
    if (at < 0) return {};
    const int from = at + needle.size();
    const int end = rec.indexOf('"', from);
    if (end < 0) return {};
    return QString::fromLatin1(rec.mid(from, end - from));
}

// A Lua array of quoted strings: Key={"a","b"}. Empty when absent.
// The key has to start at a word boundary: "Exclude={" is a SUBSTRING of
// "ForceExclude={", and the two are different fields — 27 records carry
// ForceExclude, 99 carry plain Exclude, and an unanchored indexOf answered
// for whichever happened to come first in the record.
QStringList listField(const QByteArray& rec, const char* key)
{
    const QByteArray needle = QByteArray(key) + "={";
    int at = rec.indexOf(needle);
    while (at > 0 && (isalnum(uchar(rec[at - 1])) || rec[at - 1] == '_'))
        at = rec.indexOf(needle, at + 1);
    if (at < 0) return {};
    int o = 0, c = 0;
    if (!balanced(rec, at + int(strlen(key)) + 1, &o, &c)) return {};
    QStringList out;
    const QByteArray body = rec.mid(o, c - o + 1);
    for (int i = 0; i < body.size();) {
        const int q = body.indexOf('"', i);
        if (q < 0) break;
        const int e = body.indexOf('"', q + 1);
        if (e < 0) break;
        out << QString::fromLatin1(body.mid(q + 1, e - q - 1));
        i = e + 1;
    }
    return out;
}

// Which slot a category name belongs to, and what to call it. The four names
// are the game's own keys; the ids are ours and are what the builder rows use.
struct CatRule { const char* key; const char* id; const char* label; };
const CatRule kCats[] = {
    {"Headgear", "mgo_headgear",  "Headgear"},
    {"Chest",    "mgo_chest",     "Chest Gear"},
    {"Base",     "mgo_base",      "Base"},
    {"Eyes",     "mgo_accessory", "Accessory"},
};

}  // namespace

QStringList MgoGearItem::modelStems(bool female) const
{
    // THE FALLBACK JOIN, not the primary one. The game itself ships the
    // id→model mapping — every item id has a fova pack whose .fv2 names the
    // exact model (see PlayerCatalog::buildMgo), and that is what resolves an
    // item wherever the install carries the packs. This function reproduces
    // that mapping as name rules — decoded 2026-08-28 from all of the item
    // .fv2s in the real install's mgo chunk — so a partial copy without the
    // packs still resolves what it can. Where an earlier guess disagreed with
    // the packs, the packs won:
    //   * eye_?NN → gls<NN>, index preserved; gls0 is main1, the rest main0;
    //   * cms_?01/02 → cmn0/cmn1_main0 — the common SUIT'S own family, never
    //     tcl (the BDU is teb_?03's model, which the game's Defaults name);
    //   * inb/reb/teb → icl/rcl/tcl with ids 00..02 naming model 1 and id 03
    //     naming model 0 — neither numeric nor ordinal-ascending;
    //   * ins/res/tes: _?00 → <fam>0_main0, _?01 → the mask0 (helm0 for tes)
    //     piece, _?02 → <fam>1_main0;
    //   * hat5 and hat9 are main1;
    //   * the male fits of inc00/01 and tec00/01 end "_def0", and the shipped
    //     inc files are capitalised "Inf<N>_…" (the archive really does mix
    //     cases there).
    static const QRegularExpression re{
        QStringLiteral("^([a-z]{3})_([mf])(\\d+)$")};
    const QRegularExpressionMatch m = re.match(id);
    if (!m.hasMatch()) return {};
    const QString pfx = m.captured(1);
    const int n = m.captured(3).toInt();
    // THE FEMALE PAGE NEEDS A PLAIN FALLBACK. Measured in the shipped table:
    // the FEMALE Headgear list contains eight records whose own ID ends in _m —
    //   hat_m05 hat_m06 hat_m07 hat_m08 hat_m09 hat_m11 hat_m12 hat_m14
    // — which together with the sixteen hat_f* ids make up the same twenty-four
    // hats the man has. Those eight are hats shipped ONCE, with no female
    // re-fit, so the model she wears is the plain stem; and the repo's own path
    // dictionary shows /Assets/mgo/chara/hats/ carrying hat5, hat6, hat7, hat8,
    // hat11, hat12, hat14 with no "_f" sibling anywhere in that directory —
    // which is the same statement from the other side. Appending "_f"
    // unconditionally, which is what the single-stem rule did, asked for files
    // that do not exist and dropped a third of her headgear.
    QStringList tails;
    const bool idIsFemale = m.captured(2) == QLatin1String("f");
    if (female && idIsFemale) {
        // Her own fit first; the plain model only as a last resort, reached
        // when no female fit exists at all.
        tails << QStringLiteral("_def_f") << QStringLiteral("_def");
    } else if (female) {
        // An _m id on HER page wears the PLAIN model — measured against the
        // game's own join, not assumed: hat_m07.fv2 names hat7_main0_def
        // even though a hat7_main0_def_f ships (an orphaned re-fit no table
        // record references). Trying _def_f first here is how the fallback
        // came to disagree with the fova join on exactly that item. The _f
        // cut stays as the last resort for a partial copy missing the plain
        // model.
        tails << QStringLiteral("_def") << QStringLiteral("_def_f");
    } else {
        // Never _def_f on the male page — that is how a male cut once
        // reached the woman's outfit list, from the other direction.
        tails << QStringLiteral("_def");
    }
    const auto stem = [](const QString& fam, int fi, int mi,
                         const QString& tail) {
        return fam + QString::number(fi) + QStringLiteral("_main")
            + QString::number(mi) + tail;
    };

    QStringList out;
    if (pfx == QLatin1String("eye")) {
        // eye_?NN → gls<NN>, and gls0 alone is main1 (the aviators,
        // gls0_main1_def{_f}). Measured across all twelve eyewear .fv2s.
        for (const QString& t : tails)
            out << stem(QStringLiteral("gls"), n, n == 0 ? 1 : 0, t);
    } else if (pfx == QLatin1String("cms")) {
        // The common SUIT: cms_?01 → cmn0_main0, cms_?02 → cmn1_main0.
        if (n > 0)
            for (const QString& t : tails)
                out << stem(QStringLiteral("cmn"), n - 1, 0, t);
    } else if (pfx == QLatin1String("cmc")) {
        // Its chest half pairs by index — cmn<N>_chst<N> — but the two
        // genders read the number differently, measured: the male's one cmc
        // id (cmc_m01) names cmn1_chst1, the female's cmc_f01 names
        // cmn0_chst0 (her cmc_f02 names cmn1_chst1). Each gender's OWN
        // reading is offered first, the other as the fallback, so the male
        // page cannot land on the female mapping while both models are
        // present.
        const auto chst = [](int i, const QString& tail) {
            return QStringLiteral("cmn%1_chst%2%3").arg(i).arg(i).arg(tail);
        };
        for (const QString& t : tails) {
            if (female) {
                if (n > 0) out << chst(n - 1, t);
                out << chst(n, t);
            } else {
                out << chst(n, t);
                if (n > 0) out << chst(n - 1, t);
            }
        }
    } else if (pfx == QLatin1String("inb") || pfx == QLatin1String("reb")
               || pfx == QLatin1String("teb")) {
        // The class Base garments: icl/rcl/tcl. Measured over all twelve
        // ids: 00, 01 and 02 name model 1; 03 names model 0. (The game's own
        // Defaults name ?eb_?03, i.e. model 0, as each class's default.)
        const QString fam = pfx == QLatin1String("inb")
            ? QStringLiteral("icl")
            : pfx == QLatin1String("reb") ? QStringLiteral("rcl")
                                          : QStringLiteral("tcl");
        for (const QString& t : tails) out << stem(fam, n == 3 ? 0 : 1, 0, t);
    } else if (pfx == QLatin1String("ins") || pfx == QLatin1String("res")
               || pfx == QLatin1String("tes")) {
        // A suit's three entries are three different pieces: _?00 the body,
        // _?01 the head piece (mask0; helm0 for the Enforcer), _?02 the
        // second body. Measured over all eighteen suit ids.
        for (const QString& t : tails) {
            if (n == 1)
                out << pfx + QStringLiteral("0_")
                        + (pfx == QLatin1String("tes")
                               ? QStringLiteral("helm0")
                               : QStringLiteral("mask0"))
                        + t;
            else
                out << stem(pfx, n == 2 ? 1 : 0, 0, t);
        }
    } else if (pfx == QLatin1String("inc") || pfx == QLatin1String("tec")) {
        // Chest: inc → inf, tec → tec. The male fits of indexes 0 and 1 end
        // "_def0", and the shipped inf files for those two indexes are
        // capitalised "Inf<N>_…". Both spellings are offered; QHash lookups
        // are case-sensitive and the archive really does mix cases there.
        const QString fam = pfx == QLatin1String("inc") ? QStringLiteral("inf")
                                                        : QStringLiteral("tec");
        QStringList t2 = tails;
        if (!female && n <= 1) t2.prepend(QStringLiteral("_def0"));
        for (const QString& t : t2) {
            out << stem(fam, n, 0, t);
            if (pfx == QLatin1String("inc") && n <= 1)
                out << stem(QStringLiteral("Inf"), n, 0, t);
        }
    } else if (pfx == QLatin1String("hat")) {
        // hat5 and hat9 are main1; every other hat is main0.
        for (const QString& t : tails)
            out << stem(pfx, n, (n == 5 || n == 9) ? 1 : 0, t);
    } else {
        // inh/reh/teh, rec — the id's own family and index, main0.
        for (const QString& t : tails) out << stem(pfx, n, 0, t);
    }
    out.removeDuplicates();
    return out;
}

QString MgoGearItem::modelStem(bool female) const
{
    return modelStems(female).value(0);
}

const MgoGearConfig& MgoGearConfig::instance()
{
    static MgoGearConfig cache;
    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    if (cache.m_indexKey == files.constData() && cache.m_indexCount == files.size())
        return cache;
    cache.m_indexKey = files.constData();
    cache.m_indexCount = files.size();
    cache.build();
    return cache;
}

void MgoGearConfig::build()
{
    QElapsedTimer t;
    t.start();
    m_ok = false;
    m_male.clear();
    m_female.clear();
    m_colourSwatch.clear();
    m_colourType.clear();
    m_solid = 0;
    m_pattern = 0;

    const ArchiveIndex& index = ArchiveIndex::instance();
    QByteArray data;
    QString from;

    // Dev hook: parse a decoded copy from disk, so the gear tables can be
    // exercised on an install whose archives do not carry the level_asset
    // chunk. Same shape as FOXAB_AVATAR_LUA.
    {
        const QByteArray override = qgetenv("FOXAB_GEARCONFIG_LUA");
        if (!override.isEmpty()) {
            const QString path = QString::fromLocal8Bit(override);
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                data = f.readAll();
                // The PATH, not the variable's name: a rejection message that
                // says only "FOXAB_GEARCONFIG_LUA" does not say which file.
                from = path;
            }
        }
    }

    // METAL GEAR SURVIVE SHIPS A FILE OF THE SAME NAME.
    //   /Assets/mgo/level_asset/config/GearConfig.lua   — MGO3's, 416 KB
    //   /Assets/ssd/level_asset/config/GearConfig.lua   — Survive's, 53 KB
    // They are different tables for different games: Survive's declares
    // SsdGearConfig and is keyed by CLASS first (Infil={Male={Mask=…,Helmet=…,
    // Arm=…,UpperBody=…,LowerBody=…}}), MGO's declares MgoGearConfig and is
    // keyed by gender. A match on the trailing path alone therefore accepted
    // whichever of the two the index happened to list first, which on an
    // install carrying both games is not a choice at all — and Survive's file
    // does contain "Base={{Active=", so the wrong one would not have failed
    // loudly, it would have quietly built an MGO Base slot out of Survive
    // items.
    //
    // So: the path must be MGO's, AND the content must declare MgoGearConfig.
    // Either test alone would do; both together mean neither a re-layout of
    // the tree nor a same-named file elsewhere can put another game's gear on
    // this page.
    if (!data.isEmpty() && !data.contains("MgoGearConfig.")) {
        qInfo("mgo-gear: %s does not declare MgoGearConfig — ignored",
              qUtf8Printable(from));
        data.clear();
        from.clear();
    }
    if (data.isEmpty()) {
        // BOTH tests are selectors, applied per candidate — not a path match
        // that then vetoes. Taking the first path-matching entry and only THEN
        // checking the content meant one stub or re-encoded copy listed ahead
        // of the real file emptied the gear page outright, and named the wrong
        // file as the reason.
        for (const IndexedFile& f : index.files()) {
            if (!f.path.endsWith(
                    QLatin1String("/mgo/level_asset/config/GearConfig.lua"),
                    Qt::CaseInsensitive))
                continue;
            const QByteArray d = index.readFile(f);
            if (d.isEmpty()) continue;
            if (!d.contains("MgoGearConfig.")) {
                qInfo("mgo-gear: %s does not declare MgoGearConfig — skipped",
                      qUtf8Printable(f.path));
                continue;
            }
            data = d;
            from = f.path;
            break;
        }
    }
    if (data.isEmpty()) {
        m_note = QStringLiteral(
            "GearConfig.lua is not in the configured folders — MGO gear slots "
            "cannot be built without it");
        qInfo("mgo-gear: %s", qUtf8Printable(m_note));
        return;
    }

    // Colours first: every gear item's palette is a list of ids into this.
    {
        const int at = data.indexOf("MgoGearConfig.Colors");
        int o = 0, c = 0;
        if (at >= 0 && balanced(data, at, &o, &c)) {
            const QByteArray body = data.mid(o, c - o + 1);
            for (int i = 0; i < body.size();) {
                const int idAt = body.indexOf("ID=\"", i);
                if (idAt < 0) break;
                int ro = 0, rc = 0;
                // The record is the enclosing {...}; scan back to its brace.
                int b = idAt;
                while (b > 0 && body[b] != '{') --b;
                if (!balanced(body, b, &ro, &rc)) break;
                const QByteArray rec = body.mid(ro, rc - ro + 1);
                const QString cid = field(rec, "ID");
                const QString sw = field(rec, "Swatch");
                if (!cid.isEmpty()) {
                    m_colourSwatch.insert(cid, assetPathNoExt(sw));
                    // Counted from the MAP afterwards, never incremented
                    // here: 7 colour ids are declared twice in the shipped
                    // file, so counting each record gave 374 types for 367
                    // ids — and an "untyped" figure of minus seven.
                    const QString ct = field(rec, "ColorType");
                    if (!ct.isEmpty()) m_colourType.insert(cid, ct);
                }
                i = rc + 1;
            }
        }
    }

    // Gear. Every "Category={{Active=" is an array of item records, and the
    // nearest preceding Male={ / Female={ says whose it is — the file nests
    // the two genders inside each section rather than the other way round.
    QVector<QPair<int, bool>> genderAt;   // offset, isFemale
    {
        int i = 0;
        while (i < data.size()) {
            const int m = data.indexOf("Male={", i);
            const int f = data.indexOf("Female={", i);
            // "Female={" also contains "male={", so the female match wins when
            // it starts one character earlier than the male one it contains.
            if (f >= 0 && (m < 0 || f <= m - 2 || f < m)) {
                genderAt.append({f, true});
                i = f + 8;
            } else if (m >= 0) {
                genderAt.append({m, false});
                i = m + 6;
            } else break;
        }
    }
    const auto femaleAt = [&genderAt](int p) {
        bool fem = false;
        for (const auto& g : genderAt) {
            if (g.first >= p) break;
            fem = g.second;
        }
        return fem;
    };

    // MERGED BY SLOT, DEDUPLICATED BY ID. Headgear, Base and Chest each occur
    // three times per gender because the Gears section is keyed by class
    // first — Infil, Recon, Tech — and the three copies overlap: every class
    // lists the same 24 hats and the same common suit alongside its own four
    // or five items. Appending each block as its own category is what put
    // three "Headgear" rows on the page, each 32 long, 42 of the 96 distinct.
    //
    // First record wins, and the game's own order inside a block is kept, so
    // the merged list reads as the first class's list with the other two
    // classes' exclusives appended — which is the order the file itself
    // implies and is stable across runs.
    int items = 0, dupes = 0;
    QHash<QString, MgoGearCategory> bySlot[2];   // [0]=male, [1]=female
    QVector<QString> slotOrder[2];
    // PER SLOT, not per gender. Every one of the folded duplicates is a CLASS
    // duplicate within one slot — measured over the shipped file, cross-slot
    // id overlap is exactly zero, and the nine FullSuit items per gender are
    // distinct ids that sit in different slots (ins_m01/res_m01/tes_m01 under
    // Headgear, ins_m00/ins_m02/… under Base). A per-gender set folds the same
    // 122 and gives the same 87/88 today, but it would silently delete an item
    // from Base or Chest the moment one id did appear in two slots, and report
    // the loss as a class duplicate. Scoping it to the slot cannot.
    QHash<QString, QSet<QString>> idSeen[2];
    for (const CatRule& r : kCats) {
        const QByteArray needle = QByteArray(r.key) + "={{Active=";
        int from = 0;
        while (true) {
            const int at = data.indexOf(needle, from);
            if (at < 0) break;
            from = at + needle.size();
            int o = 0, c = 0;
            if (!balanced(data, at + int(strlen(r.key)), &o, &c)) break;
            const QByteArray body = data.mid(o, c - o + 1);
            const int g = femaleAt(at) ? 1 : 0;
            const QString slotId = QLatin1String(r.id);
            if (!bySlot[g].contains(slotId)) {
                MgoGearCategory fresh;
                fresh.slotId = slotId;
                fresh.label = QLatin1String(r.label);
                bySlot[g].insert(slotId, fresh);
                slotOrder[g].append(slotId);
            }
            MgoGearCategory& cat = bySlot[g][slotId];
            // One record per top-level {...} inside the array.
            int i = 1;
            while (i < body.size()) {
                if (body[i] != '{') { ++i; continue; }
                int ro = 0, rc = 0;
                if (!balanced(body, i, &ro, &rc)) break;
                const QByteArray rec = body.mid(ro, rc - ro + 1);
                MgoGearItem it;
                it.id = field(rec, "ID");
                if (!it.id.isEmpty()) {
                    ++items;
                    // The three class blocks of one slot list the same 24
                    // hats and the same common suit alongside their own class
                    // items, so an id already taken in THIS slot is a class
                    // duplicate and is folded.
                    QSet<QString>& seenHere = idSeen[g][slotId];
                    if (seenHere.contains(it.id)) {
                        ++dupes;
                    } else {
                        seenHere.insert(it.id);
                        it.nameTag = field(rec, "NameLangTag");
                        it.swatch = assetPathNoExt(field(rec, "Swatch"));
                        it.defaultPrimary = field(rec, "DefaultPrimary");
                        it.defaultSecondary = field(rec, "DefaultSecondary");
                        it.primary = listField(rec, "Primary");
                        it.secondary = listField(rec, "Secondary");
                        // TWO exclusion fields, measured: 99 records carry
                        // Exclude, 27 carry ForceExclude (the BDU's names the
                        // cmn chest piece). Both mean "not worn together", so
                        // the item carries their union.
                        it.exclude = listField(rec, "Exclude");
                        for (const QString& e : listField(rec, "ForceExclude"))
                            if (!it.exclude.contains(e)) it.exclude << e;
                        it.fullSuit = rec.contains("FullSuit=1");
                        // THE TWO-PIECE GARMENTS. Three Chest records carry
                        // BaseGearID naming a Base record, plus
                        // RevertToDefaultBase=1, and the pair name each other
                        // in Must and SHARE A NameLangTag — cmc_f01/cms_f01,
                        // cmc_f02/cms_f02, cmc_m01/cms_m02. They are not two
                        // items, they are one garment the config expresses in
                        // two rows, and each row brings its own colour
                        // channels. Carried here; PlayerCatalog does the merge.
                        it.baseGearId = field(rec, "BaseGearID");
                        it.must = listField(rec, "Must");
                        cat.items.append(it);
                    }
                }
                i = rc + 1;
            }
        }
    }
    for (int g = 0; g < 2; ++g)
        for (const QString& slotId : slotOrder[g]) {
            const MgoGearCategory& cat = bySlot[g].value(slotId);
            if (!cat.items.isEmpty()) (g ? m_female : m_male).append(cat);
        }

    for (auto it = m_colourType.constBegin(); it != m_colourType.constEnd();
         ++it) {
        if (it.value() == QLatin1String("Pattern")) ++m_pattern;
        else if (it.value() == QLatin1String("Solid")) ++m_solid;
    }

    m_ok = !m_male.isEmpty() || !m_female.isEmpty();
    const auto count = [](const QVector<MgoGearCategory>& v) {
        int n = 0;
        for (const MgoGearCategory& c : v) n += c.items.size();
        return n;
    };
    m_note = QStringLiteral("%1 male / %2 female item(s) in %3+%4 categor(ies), "
                            "%5 colour swatch(es) (%6 solid, %7 pattern, "
                            "%8 untyped)")
                 .arg(count(m_male))
                 .arg(count(m_female))
                 .arg(m_male.size())
                 .arg(m_female.size())
                 .arg(m_colourSwatch.size())
                 .arg(m_solid)
                 .arg(m_pattern)
                 .arg(untypedColourCount());
    qInfo("mgo-gear: %s in %lld ms — %d record(s) read, %d class-duplicate(s) "
          "folded, from %s",
          qUtf8Printable(m_note), static_cast<long long>(t.elapsed()), items, dupes,
          qUtf8Printable(from));
}

}  // namespace fox
