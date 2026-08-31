// AvatarTextures.cpp — see AvatarTextures.h.
#include "index/AvatarTextures.h"

#include <algorithm>

#include "fox/FoxHash.h"
#include "index/ArchiveIndex.h"

namespace fox {
namespace {

// The asset roots an avatar texture can live under. A Survive install carries
// the MGSV library, so /tpp/ answers far more often than /ssd/ — all three are
// probed, in this order, and the first that has the file wins.
const char* const kRoots[] = {
    "/Assets/ssd/chara/avm/Pictures",
    "/Assets/tpp/chara/avm/Pictures",
    // THREE roots, not two. The dictionary names the women's body skin under
    // both /Assets/tpp/ and /Assets/mgo/, and which one an install actually
    // CARRIES varies: on the staged MGO set only the mgo copy is present, and
    // without this root the women's set resolved to nothing at all. It is
    // probed last, so it can never shadow an ssd or tpp answer.
    "/Assets/mgo/chara/avm/Pictures",
};

// The iris maps are not avatar art. They are the SHARED human eye set every
// Fox Engine character uses, and they sit one tree over, which is why nothing
// under the avatar roots ever answered for "eye colour".
const char* const kHumanRoots[] = {
    "/Assets/ssd/common_source/chara/human/Pictures",
    "/Assets/tpp/common_source/chara/human/Pictures",
};

QString two(int n) { return QStringLiteral("%1").arg(n, 2, 10, QLatin1Char('0')); }

}  // namespace

const char* AvatarTextures::decoName(int family)
{
    switch (family) {
        case 0: return "gash";
        case 1: return "tato";
        case 2: return "fpnt";
        default: return "";
    }
}

const QVector<int>& AvatarTextures::decoIds(int family) const
{
    static const QVector<int> empty;
    switch (family) {
        case 0: return m_gash;
        case 1: return m_tato;
        case 2: return m_fpnt;
        default: return empty;
    }
}

QString AvatarTextures::resolve(const QString& dir, const QString& name) const
{
    const ArchiveIndex& index = ArchiveIndex::instance();
    for (const QString& root : m_roots) {
        const QString path = root + QLatin1Char('/') + dir + QLatin1Char('/') + name;
        if (index.findByHash(
                hashFileNameWithExtension(path + QLatin1String(".ftex"))))
            return path;
    }
    return {};
}

QString AvatarTextures::facePath(int wrinkle, int skin) const
{
    if (m_facePrefix.isEmpty()) return {};
    return resolve(QStringLiteral("face"),
                   m_facePrefix + QStringLiteral("_v") + two(wrinkle)
                       + QStringLiteral("_c") + two(skin)
                       + QStringLiteral("_bsm"));
}

QString AvatarTextures::facePathFor(const QString& modelStem, int wrinkle,
                                   int skin) const
{
    // avm0_type3_def -> the avm face grid. Only the gender travels: the face
    // TEXTURE family is type0 for both genders (the other seven types ship a
    // head mesh and no map of their own), so the type digit in the stem picks
    // the model, never the texture.
    const QString g = modelStem.left(3);
    if (g == QLatin1String("avf") || g == QLatin1String("avm")) {
        bool haveGender = false;
        for (const QString& pre : m_facePrefixes) {
            if (!pre.startsWith(g)) continue;
            haveGender = true;
            const QString p = resolve(
                QStringLiteral("face"),
                pre + QStringLiteral("_v") + two(wrinkle)
                    + QStringLiteral("_c") + two(skin)
                    + QStringLiteral("_bsm"));
            if (!p.isEmpty()) return p;
        }
        // This gender HAS a grid and this cell is not in it. Returning the
        // other gender's map here would put a woman's face on a man, which is
        // the exact bug this function exists to stop — leave the material on
        // its own placeholder instead.
        if (haveGender) return {};
    }
    return facePath(wrinkle, skin);
}

QString AvatarTextures::faceNormalPathFor(const QString& modelStem,
                                         int wrinkle) const
{
    const QString g = modelStem.left(3);
    QStringList tries;
    for (const QString& pre : m_facePrefixes)
        if ((g != QLatin1String("avf") && g != QLatin1String("avm"))
            || pre.startsWith(g))
            tries << pre;
    if (tries.isEmpty() && !m_facePrefix.isEmpty()) tries << m_facePrefix;
    for (const QString& pre : tries) {
        const QString p = resolve(QStringLiteral("face"),
                                  pre + QStringLiteral("_v") + two(wrinkle)
                                      + QStringLiteral("_def_nrm"));
        if (!p.isEmpty()) return p;
    }
    return {};
}

// The face SRM. Gender-scoped exactly like facePathFor — putting a woman's
// occlusion map on a man is the same mistake as putting her colour map there —
// and the wrinkle is TRIED FIRST and then fallen back to v00, because only v00
// ships and an install that shipped more would otherwise be ignored.
QString AvatarTextures::faceSpecularPathFor(const QString& modelStem,
                                            int wrinkle) const
{
    const QString g = modelStem.left(3);
    QStringList tries;
    for (const QString& pre : m_facePrefixes)
        if ((g != QLatin1String("avf") && g != QLatin1String("avm"))
            || pre.startsWith(g))
            tries << pre;
    if (tries.isEmpty() && !m_facePrefix.isEmpty()) tries << m_facePrefix;
    for (const QString& pre : tries)
        for (const int v : {wrinkle, 0}) {
            if (v < 0) continue;
            const QString p = resolve(QStringLiteral("face"),
                                      pre + QStringLiteral("_v") + two(v)
                                          + QStringLiteral("_def_srm"));
            if (!p.isEmpty()) return p;
        }
    return {};
}

// Brows and beard: shape only, no colour. The base map is
// <pre>_<shape>_<colour>_bsm_alp and the SRM beside it is <pre>_<shape>_srm.
QString AvatarTextures::browSpecularPath(int shapeIdx) const
{
    if (m_browPrefix.isEmpty() || shapeIdx < 0 || shapeIdx >= m_browShapes.size())
        return {};
    return resolve(QStringLiteral("ebrw"),
                   m_browPrefix + QLatin1Char('_') + m_browShapes[shapeIdx]
                       + QStringLiteral("_srm"));
}

QString AvatarTextures::beardSpecularPath(int shapeIdx) const
{
    if (m_beardPrefix.isEmpty() || shapeIdx < 0
        || shapeIdx >= m_beardShapes.size())
        return {};
    return resolve(QStringLiteral("berd"),
                   m_beardPrefix + QLatin1Char('_') + m_beardShapes[shapeIdx]
                       + QStringLiteral("_srm"));
}

// The hair SRM. Same style-letter extraction and the same three folders as
// hairPath, minus the colour — which is the whole point: one SRM serves every
// colour of a hairstyle.
QString AvatarTextures::hairSpecularPathFor(const QString& modelStem,
                                            int onlyFamily) const
{
    const int at = modelStem.indexOf(QLatin1String("_hair"));
    if (at < 0) return {};
    const QString gender = modelStem.left(at);
    QString style;
    for (int i = at + 5; i < modelStem.size(); ++i) {
        const QChar c = modelStem.at(i);
        if (c == QLatin1Char('_')) continue;
        style.append(c);
        if (style.size() == 2) break;
    }
    if (style.size() != 2) return {};
    for (const char* dir : {"hair", "hrbs", "hrdc"})
        for (int fam = 0; fam < 2; ++fam) {
            if (onlyFamily >= 0 && fam != onlyFamily) continue;
            const QString p = resolve(
                QLatin1String(dir),
                gender + QStringLiteral("_hair") + QString::number(fam)
                    + QLatin1Char('_') + style + QStringLiteral("_srm"));
            if (!p.isEmpty()) return p;
        }
    return {};
}

QString AvatarTextures::browPath(int shapeIdx, int colourIdx) const
{
    if (m_browPrefix.isEmpty() || shapeIdx < 0 || shapeIdx >= m_browShapes.size()
        || colourIdx < 0 || colourIdx >= m_hairColours.size())
        return {};
    return resolve(QStringLiteral("ebrw"),
                   m_browPrefix + QLatin1Char('_') + m_browShapes[shapeIdx]
                       + QLatin1Char('_') + m_hairColours[colourIdx]
                       + QStringLiteral("_bsm_alp"));
}

QString AvatarTextures::beardPath(int shapeIdx, int colourIdx) const
{
    if (m_beardPrefix.isEmpty() || shapeIdx < 0
        || shapeIdx >= m_beardShapes.size() || colourIdx < 0
        || colourIdx >= m_hairColours.size())
        return {};
    return resolve(QStringLiteral("berd"),
                   m_beardPrefix + QLatin1Char('_') + m_beardShapes[shapeIdx]
                       + QLatin1Char('_') + m_hairColours[colourIdx]
                       + QStringLiteral("_bsm_alp"));
}

int AvatarTextures::beardIndexOf(int family, int variant) const
{
    if (family < 0 || family > 25 || variant < 0) return -1;
    const QString want = QString(QLatin1Char('a' + family))
        + QString::number(variant);
    return m_beardShapes.indexOf(want);
}

QString AvatarTextures::beardSkinPath(int shapeIdx, int colourIdx) const
{
    if (m_beardSkinPrefix.isEmpty() || shapeIdx < 0
        || shapeIdx >= m_beardShapes.size() || colourIdx < 0
        || colourIdx >= m_hairColours.size())
        return {};
    return resolve(QStringLiteral("berd"),
                   m_beardSkinPrefix + QLatin1Char('_')
                       + m_beardShapes[shapeIdx] + QLatin1Char('_')
                       + m_hairColours[colourIdx]
                       + QStringLiteral("_bsm_alp"));
}

QString AvatarTextures::hairPath(const QString& modelStem, int colourIdx,
                                int onlyFamily) const
{
    if (colourIdx < 0 || colourIdx >= m_hairColours.size()) return {};
    // The model is avf_hair_a0_v0_cov; the texture family is avf_hair0_a0.
    // The stem carries the style letter, and that is the only part that
    // travels between the two names.
    const int at = modelStem.indexOf(QLatin1String("_hair"));
    if (at < 0) return {};
    const QString gender = modelStem.left(at);        // "avf"
    QString style;
    for (int i = at + 5; i < modelStem.size(); ++i) {
        const QChar c = modelStem.at(i);
        if (c == QLatin1Char('_')) continue;
        style.append(c);
        if (style.size() == 2) break;
    }
    if (style.size() != 2) return {};
    // Two families ship (hair0 and hair1); take whichever has this colour.
    // The FOLDER is not the same for both genders: the women's hair maps are
    // under Pictures/hair, the men's under Pictures/hrbs (hair0 under hrdc).
    // Probing only "hair" is why a man's hair came back white.
    for (const char* dir : {"hair", "hrbs", "hrdc"})
        for (int fam = 0; fam < 2; ++fam) {
            if (onlyFamily >= 0 && fam != onlyFamily) continue;
            const QString p = resolve(
                QLatin1String(dir),
                gender + QStringLiteral("_hair") + QString::number(fam)
                    + QLatin1Char('_') + style + QLatin1Char('_')
                    + m_hairColours[colourIdx] + QStringLiteral("_bsm_alp"));
            if (!p.isEmpty()) return p;
        }
    return {};
}

QString AvatarTextures::hairSkinPath(const QString& modelStem,
                                     int colourIdx) const
{
    // Family 1 ONLY, never the fallback to family 0. hair0 is the card atlas
    // that belongs on the hair MESH; hair1 is the same hairstyle painted in the
    // FACE's UV layout, and putting one where the other belongs is not a
    // degraded result, it is a wrong one.
    return hairPath(modelStem, colourIdx, 1);
}

QString AvatarTextures::anyHairPath(int colourIdx) const
{
    if (colourIdx < 0 || colourIdx >= m_hairColours.size()) return {};
    for (const char* g : {"avf", "avm"})
        for (const char* dir : {"hair", "hrbs", "hrdc"})
            for (int fam = 0; fam < 2; ++fam)
                for (const char* style : {"a0", "b0", "c0", "d0"}) {
                    const QString p = resolve(
                        QLatin1String(dir),
                        QLatin1String(g) + QStringLiteral("_hair")
                            + QString::number(fam) + QLatin1Char('_')
                            + QLatin1String(style) + QLatin1Char('_')
                            + m_hairColours[colourIdx]
                            + QStringLiteral("_bsm_alp"));
                    if (!p.isEmpty()) return p;
                }
    return {};
}

QString AvatarTextures::decoPath(int family, int id, int skin) const
{
    const char* fam = decoName(family);
    if (!*fam || id < 0) return {};
    // The feature maps are per skin tone — that is the whole reason there are
    // five of each. Fall back to c00 when this install has fewer.
    for (int s : {skin, 0}) {
        const QString p = resolve(
            QStringLiteral("acce"),
            m_decoPrefix + QLatin1Char('_') + QLatin1String(fam)
                + QStringLiteral("0_v") + two(id) + QStringLiteral("_c") + two(s)
                + QStringLiteral("_bsm_alp"));
        if (!p.isEmpty()) return p;
    }
    return {};
}

// The eight shipped irises, named by decoding cm_iris0…7_c00_bsm and looking
// at them. The order is the archive's own (the digit in the file name), which
// is also the order the game's own eyecolor tiles are numbered in — verified
// tile-for-texture across all eight.
const char* AvatarTextures::irisName(int colourIdx)
{
    static const char* const kNames[] = {"Blue",  "Amber", "Brown", "Grey",
                                         "Green", "Hazel", "Red",   "White"};
    if (colourIdx < 0 || colourIdx >= int(sizeof(kNames) / sizeof(kNames[0])))
        return "";
    return kNames[colourIdx];
}

QString AvatarTextures::irisPath(int colourIdx, int shade) const
{
    if (colourIdx < 0 || !m_irisColours.contains(colourIdx)) return {};
    // Try the shade asked for, then the shared shade list, then ANY shade this
    // colour ships. The last step matters: the colour list and the shade list
    // are gated differently on purpose (a colour counts if it ships any shade,
    // a shade counts only if every colour ships it), so on a copy with one
    // print of one colour missing there is a colour in the row whose entry in
    // the shared shade list does not exist. Falling back per colour keeps that
    // row working instead of resolving to nothing and silently doing nothing.
    QVector<int> tries;
    tries.append(shade);
    for (int sh : m_irisShades)
        if (!tries.contains(sh)) tries.append(sh);
    for (int sh = 0; sh < 8; ++sh)
        if (!tries.contains(sh)) tries.append(sh);
    const ArchiveIndex& index = ArchiveIndex::instance();
    for (int sh : tries) {
        if (sh < 0) continue;
        const QString name = QStringLiteral("cm_iris")
            + QString::number(colourIdx) + QStringLiteral("_c") + two(sh)
            + QStringLiteral("_bsm");
        for (const QString& root : m_humanRoots) {
            const QString path = root + QLatin1Char('/') + name;
            if (index.findByHash(
                    hashFileNameWithExtension(path + QLatin1String(".ftex"))))
                return path;
        }
    }
    return {};
}

QString AvatarTextures::bodyPath(int skin, bool men) const
{
    // This gender's set first, the other only as a fallback — an install that
    // ships one of the two is better served by the wrong skin than by a bare
    // limb in whatever tone its model happened to be authored in, but an
    // install that ships both must never be.
    const QString order[2] = { men ? m_bodyPrefixMen : m_bodyPrefix,
                               men ? m_bodyPrefix : m_bodyPrefixMen };
    for (const QString& prefix : order) {
        if (prefix.isEmpty()) continue;
        for (int s : {skin, 0}) {
            const QString p = resolve(QStringLiteral("body"),
                                      prefix + QStringLiteral("_c") + two(s)
                                          + QStringLiteral("_bsm"));
            if (!p.isEmpty()) return p;
        }
    }
    return {};
}

const AvatarTextures& AvatarTextures::instance()
{
    static AvatarTextures c;
    static const void* key = nullptr;
    const void* now = ArchiveIndex::instance().files().constData();
    if (now != key) {
        key = now;
        c.build();
    }
    return c;
}

void AvatarTextures::build()
{
    m_roots.clear();
    m_facePrefix.clear();
    m_facePrefixes.clear();
    m_browPrefix.clear();
    m_beardPrefix.clear();
    m_beardSkinPrefix.clear();
    m_beardSkinShapes = 0;
    m_beardShapes.clear();
    m_decoPrefix.clear();
    m_wrinkles.clear();
    m_skins.clear();
    m_browShapes.clear();
    m_hairColours.clear();
    m_gash.clear();
    m_tato.clear();
    m_fpnt.clear();
    m_bodyPrefix.clear();
    m_bodyPrefixMen.clear();
    m_bodyTones.clear();
    m_humanRoots.clear();
    m_irisColours.clear();
    m_irisShades.clear();
    m_note.clear();

    const ArchiveIndex& index = ArchiveIndex::instance();
    if (index.files().isEmpty()) return;
    for (const char* r : kRoots) m_roots.append(QLatin1String(r));

    const auto exists = [&](const QString& dir, const QString& name) {
        return !resolve(dir, name).isEmpty();
    };
    // Every <letter><digit> shape under `dir`/`prefix` that ships in at least
    // two of the known colours. Shared by the brows and the beards.
    const auto shapesIn = [&](const QString& dir, const QString& prefix) {
        QStringList out;
        if (prefix.isEmpty()) return out;
        for (char letter = 'a'; letter <= 'l'; ++letter)
            for (int n = 0; n < 5; ++n) {
                const QString shape = QString(QLatin1Char(letter))
                    + QString::number(n);
                int hits = 0;
                for (const QString& col : m_hairColours)
                    if (exists(dir, prefix + QLatin1Char('_') + shape
                                        + QLatin1Char('_') + col
                                        + QStringLiteral("_bsm_alp"))
                        && ++hits >= 2)
                        break;
                if (hits >= 2) out.append(shape);
            }
        return out;
    };

    // ── Face: the prefix for EVERY gender that ships one, then v and c ──────
    // Both avf and avm ship a face grid and a character must be given its own.
    // The first one found is the fallback for anything whose gender cannot be
    // read off the model stem.
    for (const char* g : {"avf", "avm"}) {
        QString found;
        for (int n = 0; n < 4 && found.isEmpty(); ++n)
            for (int t = 0; t < 8; ++t) {
                const QString pre = QLatin1String(g) + QString::number(n)
                    + QStringLiteral("_type") + QString::number(t);
                if (exists(QStringLiteral("face"),
                           pre + QStringLiteral("_v00_c00_bsm"))) {
                    found = pre;
                    break;
                }
            }
        if (found.isEmpty()) continue;
        m_facePrefixes.append(found);
        if (m_facePrefix.isEmpty()) m_facePrefix = found;
    }
    // Skin tones first, off the base wrinkle set, because the tone grid is what
    // tells a real wrinkle set apart from an impostor.
    //
    // A WRINKLES TYPE entry ships the WHOLE tone grid. The AVATAR/SKIN screen
    // crosses "Wrinkles Type" with "Skin Color", so every selectable wrinkle
    // level has to exist in every tone or the screen would have holes in it —
    // and measured against the retail archives, v00…v07 each ship all five
    // tones for both genders while the female's v08 ships c00 and c04 only.
    // Decoding v08 shows why: it is not an aged version of the same face at
    // all, it is a different head entirely (different UV art, bared teeth, no
    // brows), so it is some other asset that happens to sit in the same number
    // series. Eight real sets is also exactly the size of the game's own tile
    // pack (ui_avatar_f_preset_skincolor_1…8) and exactly what the male ships,
    // which is why the browser's ninth row had no thumbnail: there is no ninth
    // wrinkle type.
    //
    // The rule is measured, not a hard-coded 8 — a build that ships a tenth
    // complete set gets a tenth row. Anything dropped is logged rather than
    // silently swallowed.
    // ACROSS EVERY FACE PREFIX, not just the first one found. m_facePrefix is
    // whichever family the scan above happened to reach first, and the scan
    // tries "avf" before "avm" — so on an install where the avf grid is thin
    // (Survive's female head ships one tone and no brows here) every character
    // was offered one skin tone and no facial hair, the MGO male included,
    // because the counts were taken off a head he does not wear.
    //
    // The union is the right set precisely because facePathFor() already tries
    // every prefix in turn: a tone that resolves under any of them is a tone
    // the screen can actually show. This is the same correction the body
    // prefix needed for the same reason, a few dozen lines down.
    if (!m_facePrefixes.isEmpty()) {
        const auto anyPrefixHas = [&](const QString& suffix) {
            for (const QString& pre : m_facePrefixes)
                if (exists(QStringLiteral("face"), pre + suffix)) return true;
            return false;
        };
        for (int s = 0; s < 12; ++s)
            if (anyPrefixHas(QStringLiteral("_v00_c") + two(s)
                             + QStringLiteral("_bsm")))
                m_skins.append(s);
        QVector<int> partial;
        for (int v = 0; v < 16; ++v) {
            if (!anyPrefixHas(QStringLiteral("_v") + two(v)
                              + QStringLiteral("_c00_bsm")))
                continue;
            // A wrinkle set is complete when ONE prefix carries the whole tone
            // grid for it. Allowing the tones to be satisfied by different
            // prefixes would call a set complete that no single head can show.
            bool full = false;
            for (const QString& pre : m_facePrefixes) {
                bool all = true;
                for (int s : m_skins)
                    if (!exists(QStringLiteral("face"),
                                pre + QStringLiteral("_v") + two(v)
                                    + QStringLiteral("_c") + two(s)
                                    + QStringLiteral("_bsm"))) {
                        all = false;
                        break;
                    }
                if (all) { full = true; break; }
            }
            if (full)
                m_wrinkles.append(v);
            else
                partial.append(v);
        }
        if (!partial.isEmpty()) {
            QStringList names;
            for (int v : partial)
                names << (m_facePrefix + QStringLiteral("_v") + two(v));
            qInfo("avatar-tex: %lld face set(s) ship only part of the %lld-tone "
                  "grid and are not wrinkle types: %s",
                  qint64(partial.size()), qint64(m_skins.size()),
                  qUtf8Printable(names.join(QStringLiteral(", "))));
        }
    }

    // ── Hair colours: the shared colour vocabulary for hair, brows and beards.
    //
    // THE ORDER IS THE GAME'S, NOT ALPHABETICAL, and it matters: a preset
    // stores a colour as an index into this list, so listing them in any other
    // order silently gives every preset the wrong hair. Measured by decoding
    // all 28 shipped preset thumbnails
    // (/Assets/ssd/ui/texture/Avatar_mgo/base/ui_avatar_f_preset_base_1…28)
    // and reading the hair colour off each, then comparing with the colour code
    // in avatar_presets_women. 28 of 28 agree on this order and on no other:
    //
    //   0 gd0 blonde   1 br0 brown   2 bk0 black   3 wh0 white   4 rd0 red
    //
    // The trailing three are not used by any female preset and are appended
    // after the five so they cannot disturb the indices; the men's beard set
    // adds bl0 on top of those.
    static const char* const kColours[] = {"gd0", "br0", "bk0", "wh0", "rd0",
                                           "gr0", "sv0", "pk0", "bl0"};

    // ── Eyebrows: prefix, then shapes and colours ───────────────────────────
    for (const char* g : {"avm", "avf"}) {
        for (int n = 0; n < 3 && m_browPrefix.isEmpty(); ++n) {
            const QString pre = QLatin1String(g) + QStringLiteral("_ebrw")
                + QString::number(n);
            for (const char* col : kColours)
                if (exists(QStringLiteral("ebrw"),
                           pre + QStringLiteral("_a0_") + QLatin1String(col)
                               + QStringLiteral("_bsm_alp"))) {
                    m_browPrefix = pre;
                    break;
                }
        }
        if (!m_browPrefix.isEmpty()) break;
    }
    if (!m_browPrefix.isEmpty()) {
        for (const char* col : kColours)
            if (exists(QStringLiteral("ebrw"),
                       m_browPrefix + QStringLiteral("_a0_") + QLatin1String(col)
                           + QStringLiteral("_bsm_alp")))
                m_hairColours.append(QLatin1String(col));
        // A shape counts when it ships in at least TWO colours. The archives
        // carry one stray half-set — avm_ebrw0_a1 exists in gd0 and in no other
        // colour — and letting it in makes twelve brow shapes out of eleven,
        // sliding every preset's brow (and every eyebrow thumbnail, which is
        // numbered by list position) one place along.
        //
        // "At least two" rather than "all": the colour list is itself derived
        // from what shape a0 happens to ship, so demanding the full set would
        // let one colour that is unique to a0 veto every other shape. Two is
        // enough to tell a real shape from a stray and cannot collapse the list.
        m_browShapes = shapesIn(QStringLiteral("ebrw"), m_browPrefix);
    }

    // ── Beards: men only, and a much larger shape set than the brows ────────
    // a0…a3, then b0…j2 — 31 shapes against the brows' 11, over the same
    // colour vocabulary plus a few the women's sets never use.
    for (const char* g : {"avm", "avf"}) {
        for (int n = 0; n < 3 && m_beardPrefix.isEmpty(); ++n) {
            const QString pre = QLatin1String(g) + QStringLiteral("_berd")
                + QString::number(n);
            for (const char* col : kColours)
                if (exists(QStringLiteral("berd"),
                           pre + QStringLiteral("_a0_") + QLatin1String(col)
                               + QStringLiteral("_bsm_alp"))) {
                    m_beardPrefix = pre;
                    break;
                }
        }
        if (!m_beardPrefix.isEmpty()) break;
    }
    // Same two-colour rule as the brows, and for the same reason: letting one
    // arbitrary colour decide the shape set means the shape COUNT depends on
    // which colour is probed first, and the count is what the UI numbers by.
    if (!m_beardPrefix.isEmpty())
        m_beardShapes = shapesIn(QStringLiteral("berd"), m_beardPrefix);

    // The colour vocabulary is shared by hair, brows and beards, but it must
    // not DEPEND on the brows: an install with hair and no eyebrow set would
    // otherwise report no hair colours at all and leave the hair white.
    for (const char* g : {"avf", "avm"})
        for (int fam = 0; fam < 3; ++fam)
            for (const char* style : {"a0", "b0", "c0", "d0"})
                for (const char* col : kColours) {
                    if (m_hairColours.contains(QLatin1String(col))) continue;
                    for (const char* dir : {"hair", "hrbs", "hrdc"})
                        if (exists(QLatin1String(dir),
                                   QLatin1String(g) + QStringLiteral("_hair")
                                       + QString::number(fam) + QLatin1Char('_')
                                       + QLatin1String(style) + QLatin1Char('_')
                                       + QLatin1String(col)
                                       + QStringLiteral("_bsm_alp"))) {
                            m_hairColours.append(QLatin1String(col));
                            break;
                        }
                }

    // ── The SECOND beard layer ──────────────────────────────────────────────
    //
    // avm_berd0 and avm_berd1 are not two alternative beard sets — they are the
    // two halves of one beard, and the browser was drawing half of it. berd0 is
    // the strand-card ATLAS that goes on the beard MESH (rows of loose hair
    // cards, which is why it made such a poor icon); berd1 is the same beard
    // painted in the FACE's own UV layout, and laying it over the face map
    // drops a full beard, a moustache or a pair of mutton chops exactly where
    // they belong — verified by compositing berd1_a2, _c0 and _e0 onto
    // avm0_type0_v00_c00_bsm. NOTHING in either head model binds it, which is
    // why reading the models never turned it up: the game composites it at
    // runtime, the same way it does a scar.
    //
    // This runs AFTER the hair-colour vocabulary is complete. The brow set
    // fills that list first but not fully — the beard set uses colours the
    // brows do not — so probing here rather than beside the berd0 enumeration
    // is what lets a beard-only colour answer.
    if (!m_beardPrefix.isEmpty() && !m_beardShapes.isEmpty()
        && m_beardPrefix.endsWith(QLatin1Char('0'))) {
        const QString cand = m_beardPrefix.left(m_beardPrefix.size() - 1)
            + QLatin1Char('1');
        for (const QString& col : m_hairColours)
            if (exists(QStringLiteral("berd"),
                       cand + QLatin1Char('_') + m_beardShapes.first()
                           + QLatin1Char('_') + col
                           + QStringLiteral("_bsm_alp"))) {
                m_beardSkinPrefix = cand;
                break;
            }
    }
    // Both prefixes are authored in parallel — the dictionary lists the same 31
    // shape names and the same eight colours under each — so a shape index
    // addresses both layers. Say so out loud rather than assuming it: a shape
    // with no skin layer renders as beard cards on a clean-shaven face, and
    // that is the exact bug this exists to fix, so it must not fail quietly.
    if (!m_beardSkinPrefix.isEmpty()) {
        QStringList meshOnly;
        for (const QString& sh : m_beardShapes) {
            bool any = false;
            for (const QString& col : m_hairColours)
                if (exists(QStringLiteral("berd"),
                           m_beardSkinPrefix + QLatin1Char('_') + sh
                               + QLatin1Char('_') + col
                               + QStringLiteral("_bsm_alp"))) {
                    any = true;
                    break;
                }
            if (!any) meshOnly.append(sh);
        }
        m_beardSkinShapes = m_beardShapes.size() - meshOnly.size();
        if (!meshOnly.isEmpty())
            qInfo("avatar-tex: %lld beard shape(s) ship the mesh atlas but no "
                  "skin layer, so they draw as cards on bare skin: %s",
                  qint64(meshOnly.size()),
                  qUtf8Printable(meshOnly.join(QStringLiteral(", "))));
    }

    // ── Facial features: gash / tato / fpnt ─────────────────────────────────
    for (const char* g : {"avm", "avf"}) {
        if (exists(QStringLiteral("acce"),
                   QLatin1String(g) + QStringLiteral("_gash0_v00_c00_bsm_alp"))
            || exists(QStringLiteral("acce"),
                      QLatin1String(g)
                          + QStringLiteral("_tato0_v00_c00_bsm_alp"))) {
            m_decoPrefix = QLatin1String(g);
            break;
        }
    }
    if (!m_decoPrefix.isEmpty()) {
        for (int fam = 0; fam < 3; ++fam) {
            QVector<int>& into = fam == 0 ? m_gash : fam == 1 ? m_tato : m_fpnt;
            for (int v = 0; v < 16; ++v)
                if (exists(QStringLiteral("acce"),
                           m_decoPrefix + QLatin1Char('_')
                               + QLatin1String(decoName(fam))
                               + QStringLiteral("0_v") + two(v)
                               + QStringLiteral("_c00_bsm_alp")))
                    into.append(v);
        }
    }

    // ── Body skin: the same five tones as the face, shared by every bare limb.
    //
    // ONE PREFIX PER GENDER. Both ship — avf0_body0_def_c00…c04 and
    // avm0_body0_def_c00…c04, side by side under /chara/avm/Pictures/body — but
    // the old loop kept ONE prefix, tried "avf" first and stopped at the first
    // hit. So whichever set an install happened to resolve served everybody:
    // where avf answers, every bare MALE limb wore the women's skin; where it
    // does not, every bare female limb wore the men's. Which way round it went
    // was decided by which files that install carried, which is why it reads
    // as random.
    for (const char* g : {"avf", "avm"}) {
        const bool men = QLatin1String(g) == QLatin1String("avm");
        QString& into = men ? m_bodyPrefixMen : m_bodyPrefix;
        for (int n = 0; n < 3 && into.isEmpty(); ++n) {
            const QString pre = QLatin1String(g) + QString::number(n)
                + QStringLiteral("_body0_def");
            if (exists(QStringLiteral("body"), pre + QStringLiteral("_c00_bsm")))
                into = pre;
        }
    }
    // The tone list is the union: it drives nothing but the note, and reporting
    // the smaller of two sets would understate what the install carries.
    for (const QString& pre : {m_bodyPrefix, m_bodyPrefixMen}) {
        if (pre.isEmpty()) continue;
        for (int s = 0; s < 12; ++s)
            if (!m_bodyTones.contains(s)
                && exists(QStringLiteral("body"),
                          pre + QStringLiteral("_c") + two(s)
                              + QStringLiteral("_bsm")))
                m_bodyTones.append(s);
    }
    std::sort(m_bodyTones.begin(), m_bodyTones.end());

    // ── Iris colours: the shared human eye set, one map per colour and shade.
    //
    // These are the "Right Eye Color" / "Left Eye Color" rows on the AVATAR
    // screen. They are NOT avatar art and nothing under /chara/avm names them —
    // the game re-points one texture slot on each of the head's two eye
    // materials at /common_source/chara/human/Pictures/cm_iris<N>_c<SS>_bsm.
    // Retail ships eight colours (0 blue, 1 amber, 2 brown, 3 grey, 4 green,
    // 5 hazel, 6 red, 7 white — decoded, in that order) in two shades each,
    // c00 bright and c01 dark, plus one stray cm_iris0_c04 with no siblings.
    // Only shades that EVERY colour ships are offered, so the row cannot have
    // a hole in it.
    {
        const auto irisExists = [&](int n, int sh) {
            for (const char* root : kHumanRoots) {
                const QString path = QLatin1String(root)
                    + QStringLiteral("/cm_iris") + QString::number(n)
                    + QStringLiteral("_c") + two(sh) + QStringLiteral("_bsm");
                if (index.findByHash(hashFileNameWithExtension(
                        path + QLatin1String(".ftex")))) {
                    if (!m_humanRoots.contains(QLatin1String(root)))
                        m_humanRoots.append(QLatin1String(root));
                    return true;
                }
            }
            return false;
        };
        // A colour counts when ANY of its shades ships — not specifically c00.
        // Gating on c00 looked reasonable and was wrong: an install can be
        // missing exactly one print of one colour (this machine's TPP copy has
        // cm_iris6 in c01 only), and that would silently drop a colour the game
        // offers rather than dropping the shade that is actually absent.
        for (int n = 0; n < 16; ++n)
            for (int sh = 0; sh < 8; ++sh)
                if (irisExists(n, sh)) { m_irisColours.append(n); break; }
        if (!m_irisColours.isEmpty())
            for (int sh = 0; sh < 8; ++sh) {
                bool all = true;
                for (int n : m_irisColours)
                    if (!irisExists(n, sh)) { all = false; break; }
                if (all) m_irisShades.append(sh);
            }
    }

    if (!ok()) {
        m_note = QStringLiteral(
            "no avatar face textures in the configured folders");
        return;
    }
    m_note = QStringLiteral(
                 "%1: %2 wrinkle set(s) x %3 skin tone(s); %4 brow shape(s) x "
                 "%5 colour(s); features %6 scar / %7 tattoo / %8 paint")
                 .arg(m_facePrefix)
                 .arg(m_wrinkles.size())
                 .arg(m_skins.size())
                 .arg(m_browShapes.size())
                 .arg(m_hairColours.size())
                 .arg(m_gash.size())
                 .arg(m_tato.size())
                 .arg(m_fpnt.size())
             + (m_beardShapes.isEmpty()
                    ? QString()
                    : QStringLiteral("; %1 beard shape(s), %2 with a skin layer")
                          .arg(m_beardShapes.size())
                          .arg(m_beardSkinShapes))
             + (m_bodyTones.isEmpty()
                    ? QStringLiteral("; no shared body skin")
                    : QStringLiteral("; %1 body tone(s) (%2)")
                          .arg(m_bodyTones.size())
                          .arg(m_bodyPrefix.isEmpty()
                                   ? QStringLiteral("men only")
                                   : m_bodyPrefixMen.isEmpty()
                                       ? QStringLiteral("women only")
                                       : QStringLiteral("both genders")))
             + (m_irisColours.isEmpty()
                    ? QStringLiteral("; no iris maps")
                    : QStringLiteral("; %1 iris colour(s) x %2 shade(s)")
                          .arg(m_irisColours.size())
                          .arg(m_irisShades.size()));
    qInfo("avatar-tex: %s", qUtf8Printable(m_note));
}

}  // namespace fox
