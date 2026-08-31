// AvatarPresets.cpp — see AvatarPresets.h.
#include "index/AvatarPresets.h"

#include <QFile>
#include <QStringList>

#include "index/ArchiveIndex.h"
#include "index/AvatarTextures.h"

namespace fox {
namespace {

// 128 + k is how the table encodes "option k"; 255 means "none". Anything else
// is a raw index (the deco tables use small plain numbers).
int optionOf(int raw)
{
    if (raw == 255) return -1;
    return raw >= 128 ? raw - 128 : raw;
}

// One table out of the Lua chunk, as rows of integers. Brace-counted rather
// than pattern-matched: the chunk is minified onto a single line and every
// table has a different column count.
QVector<QVector<int>> tableOf(const QString& src, const char* name)
{
    const QString key = QLatin1String(name) + QLatin1String("={{");
    int at = src.indexOf(key);
    // "eye={{" must not match inside another key. Every table is preceded by
    // '{' (the first) or ',' (the rest), so require one.
    while (at > 0 && src.at(at - 1) != QLatin1Char('{')
           && src.at(at - 1) != QLatin1Char(','))
        at = src.indexOf(key, at + 1);
    if (at < 0) return {};
    at += key.size() - 2;   // land on the outer '{'

    QVector<QVector<int>> rows;
    QVector<int> cur;
    QString num;
    int depth = 0;
    for (int i = at; i < src.size(); ++i) {
        const QChar c = src.at(i);
        if (c == QLatin1Char('{')) {
            if (++depth == 2) { cur.clear(); num.clear(); }
        } else if (c == QLatin1Char('}')) {
            if (depth == 2) {
                if (!num.isEmpty()) cur.append(num.toInt());
                rows.append(cur);
                num.clear();
            }
            if (--depth == 0) break;
        } else if (depth == 2) {
            if (c == QLatin1Char(',')) { cur.append(num.toInt()); num.clear(); }
            else if (c.isDigit() || c == QLatin1Char('-')) num.append(c);
        }
    }
    return rows;
}

int cell(const QVector<QVector<int>>& t, int row, int col)
{
    if (row < 0 || row >= t.size()) return 255;
    const QVector<int>& r = t[row];
    return (col >= 0 && col < r.size()) ? r[col] : 255;
}

}  // namespace

QString AvatarPreset::describe() const
{
    QStringList bits;
    bits << QStringLiteral("head av%10_type%2")
                .arg(women ? QLatin1Char('f') : QLatin1Char('m'))
                .arg(faceType);
    bits << (hairMesh < 0 ? QStringLiteral("no hair")
                          : QStringLiteral("hair %1")
                                .arg(QLatin1Char('A' + char(hairMesh))));
    if (hairColour >= 0) bits << QStringLiteral("hair colour %1").arg(hairColour + 1);
    if (skinColour >= 0) bits << QStringLiteral("skin %1").arg(skinColour + 1);
    if (eyeSet >= 0) bits << QStringLiteral("eyes %1").arg(eyeSet + 1);
    if (eyeColourR >= 0 || eyeColourL >= 0) {
        // "-" for an eye the table leaves unset, so a one-eyed row reads as
        // "eyes -/Green" rather than "eyes /Green".
        const auto name = [](int c) {
            const QString n = QString::fromLatin1(AvatarTextures::irisName(c));
            return n.isEmpty() ? QStringLiteral("-") : n;
        };
        bits << (eyeColourR == eyeColourL
                     ? QStringLiteral("%1 eyes").arg(name(eyeColourR))
                     : QStringLiteral("eyes %1/%2")
                           .arg(name(eyeColourR), name(eyeColourL)));
    }
    if (browShape >= 0) bits << QStringLiteral("brow %1").arg(browShape + 1);
    if (beard >= 0) bits << QStringLiteral("beard %1").arg(beard + 1);
    bits << (decoType < 0 ? QStringLiteral("no feature")
                          : QStringLiteral("feature %1.%2").arg(decoType).arg(decoId));
    return bits.join(QStringLiteral(" · "));
}

QString AvatarPresets::iconPathFor(int index, Sex sex)
{
    // The grid numbers presets from 1; so do the shipped thumbnails.
    return uiIconPath("base", index + 1, sex);
}

// ── The shipped icon sets, and what their numbering means ────────────────────
//
// Counted from the name dictionary, not assumed:
//
//   base     1…28  (1-BASED)  one per face preset
//   eyeblow  0…11  (0-based)  12 slots for 11 brow shapes → 0 is "none"
//   hair      0…4  (0-based)  5 slots for 4 styles + bald → exact fit
//   deco     0…30  (0-based)  31 slots
//
// The deco set is the one worth spelling out. The archives name ten gash, ten
// tato and five fpnt features (25), which does not obviously make 31. It does
// make 31 as 1 + 3x10: slot 0 is "no feature" and each family gets a fixed
// block of ten, whether or not the game ships all ten. That is the only
// arrangement of these families that lands exactly on 31, and it also explains
// why fpnt's five shipped textures leave the last five slots dark.
//
//   deco_0            no feature
//   deco_1  … deco_10 tato v00…v09
//   deco_11 … deco_20 not shipped in Survive — MGO's own extras
//   deco_21 … deco_25 fpnt v00…v04
//   deco_26 … deco_30 gash v00…v04
//
// The blocks are NOT uniform tens, which is why arithmetic alone got this
// wrong twice. Every one of the twenty shipped features was matched to its
// thumbnail by decoding both and comparing them:
//
//   tato   preset 14's row is {tato, 7} and its preset thumbnail carries the
//          same tribal design as deco_8, mirrored — which is exactly what that
//          preset's side flag says. Anchors the block at 1.
//   fpnt   avm_fpnt0_v04 is the skull, and so is deco_25. Anchors the block
//          at 21, and v00…v03 line up with 21…24.
//   gash   composited each of the five scar maps over the face and compared:
//          v00 scattered scratches = deco_26, v01 the long branching cheek
//          scar = deco_27 (and that is preset 19's scar, whose row is
//          {gash, 1}), v02 the thin curve = deco_28, v03 the brow branching =
//          deco_29, v04 the cheek mark = deco_30. Five of five, in order.
//
// Slots 11-20 are pale marks with nothing behind them here; the folder is
// literally named Avatar_mgo and the art is MGO3's full set.
//
// Computing the slot from (family, id) rather than from a running count over
// what happens to be present keeps a partial install from shifting every icon
// after the first gap.
QString AvatarPresets::decoIconPath(int family, int id, Sex sex)
{
    // Each family's block is exactly ten slots wide. An id past the tenth has
    // no thumbnail of its own, and borrowing the next family's would be worse
    // than showing none, so fall back to the texture swatch (empty path).
    if (family < 0 || family > 2 || id < 0) return uiIconPath("deco", 0, sex);
    // family is 0 gash, 1 tato, 2 fpnt. First slot and length of each block,
    // measured (see above) rather than assumed to be evenly spaced.
    static const int kFirst[3] = {26, 1, 21};
    static const int kCount[3] = {5, 10, 5};
    if (id >= kCount[family]) return {};   // no thumbnail of its own
    return uiIconPath("deco", kFirst[family] + id, sex);
}

// The Avatar_mgo pack carries MORE sets than the community name dictionary
// lists. base, deco, eyeblow and hair are in the dictionary; `eye` and
// `skincolor` are in the pack and in no dictionary, which is why they read as
// absent. Found by hashing candidate names and matching them against the
// pack's own entry hashes:
//
//   /Assets/ssd/ui/texture/Avatar_mgo/eye/ui_avatar_f_preset_eye_1 … _8
//   /Assets/ssd/ui/texture/Avatar_mgo/skincolor/ui_avatar_f_preset_skincolor_1 … _8
//
// Eight eye tiles for eight eye shapes, numbered from 1, which is the same
// 1-based convention the 28 face-preset portraits use.
QString AvatarPresets::eyeIconPath(int shapeIdx, Sex sex)
{
    if (shapeIdx < 0 || shapeIdx > 7) return {};
    return uiIconPath("eye", shapeIdx + 1, sex);
}

// The WRINKLE tiles live in the folder called "skincolor". That is not a
// mistake in the archive and it is not skin tones: decoding all eight shows one
// face at one tone with eight different amounts of age in it, which is exactly
// what the game's AVATAR/SKIN screen draws above the row labelled "Wrinkles
// Type". The folder is named for the SCREEN, not for the row on it — which is
// why every name guessed for "wrinkle" came back empty while the art sat in a
// folder that had been dismissed as swatches.
//
// Eight tiles against the nine wrinkle sets the archives carry, so the ninth
// has no tile of its own and falls back to a crop of its own face map.
QString AvatarPresets::wrinkleIconPath(int setIdx, Sex sex)
{
    if (setIdx < 0 || setIdx > 7) return {};
    return uiIconPath("skincolor", setIdx + 1, sex);
}

// The eye-COLOUR tiles are the one set that is not in the Avatar_mgo pack, and
// looking for them there is why they read as missing: they were never moved out
// of MGO3's older icon tree, so they sit under the TPP root while the shape,
// brow, hair, feature and wrinkle tiles beside them on the same screen sit
// under the Survive one. The women's edit screen (ui_avatar_edit_women.pftxs)
// packs both roots together, which is how the pairing was confirmed — and the
// eight tiles match cm_iris0…7 one for one, checked by decoding both.
//
// 1-based, like the other tiles that have no "none" slot: there is no
// ui_avatar_preset_eyecolor_0.
QString AvatarPresets::eyeColourIconPath(int colourIdx)
{
    if (colourIdx < 0 || colourIdx > 7) return {};
    return QStringLiteral(
               "/Assets/tpp/ui/texture/Avatar/eyecolor/ui_avatar_preset_eyecolor_")
        + QString::number(colourIdx + 1);
}

QString AvatarPresets::browIconPath(int shapeIdx, Sex sex)
{
    return uiIconPath("eyeblow", shapeIdx < 0 ? 0 : shapeIdx + 1, sex);
}

// The hair pack is FIVE tiles, 0-based, and slot 0 is BALD — decoded and
// looked at: ui_avatar_f_preset_hair_0 is a hairless head, _1…_4 are the four
// shipped hair meshes in order. That is the same convention `eyeblow` and
// `deco` use (slot 0 is the "none" tile, the real choices follow), and it is
// why the browser's Hairstyle row used to read one short: bald was being given
// tile 4 — d0's art — and each mesh was wearing the tile of the mesh before it.
//
// So: bald takes tile 0, mesh k takes tile k+1, and the row is the game's own
// five options with no gap.
QString AvatarPresets::hairIconPath(int meshIdx, Sex sex)
{
    if (meshIdx < 0) return uiIconPath("hair", 0, sex);   // bald / "none"
    // Five tiles for the women (bald + four styles), four for the men (bald +
    // three) — which is exactly how many hair meshes each gender ships, so the
    // bound is the tree's, not a shared guess.
    const int last = sex == Sex::Men ? 3 : 4;
    if (meshIdx + 1 > last) return {};
    return uiIconPath("hair", meshIdx + 1, sex);
}

// The men's Beard tiles: clean-shaven is tile 0 and the five shipped FAMILIES
// follow it, in the archives' own a…e order. Seven files ship, but tile 6 is
// byte-for-byte identical to tile 0 — a padded slot, not a sixth style — so
// there are five real ones.
//
// The pairing is measured, not read off the pictures. Ranking the five families
// by how much of the face their alpha covers gives a > e > d > c > b, and
// ranking tiles 1-5 by how far each differs from the clean-shaven tile gives
// exactly the same order, 5 of 5, with the extremes matching in ratio as well
// (a/e is 2.27 by coverage and 2.28 by tile difference). The pictures agree:
// tile 1 is a full beard, tile 2 is barely visible, tile 5 runs along the jaw.
//
// `family` is 0-4 (a…e); the three density variants inside a family share one
// tile, because the game ships one tile per family and not fifteen.
QString AvatarPresets::beardIconPath(int family)
{
    if (family < -1 || family > 4) return {};
    return uiIconPath("beard", family + 1, Sex::Men);
}

// TWO icon trees ship, one per gender, and reading only the first is why the
// male page was so far behind: it was drawing the WOMEN's portraits, the
// women's eye shapes and the women's wrinkles beside a male head.
//
//   women  /Assets/ssd/ui/texture/Avatar_mgo/<set>/ui_avatar_f_preset_<set>_<n>
//   men    /Assets/tpp/ui/texture/Avatar/<set>/ui_avatar_preset_<set>_<n>
//
// The men's is MGO3's original set — no gender letter in the name, because when
// it was authored there was only one avatar — and Survive kept it where it was
// and added the women's beside it under its own root. That is also why the
// men's tree was invisible to a name-dictionary search: the dictionary covers
// Avatar_mgo and not Avatar. Recovered by hashing candidate names against the
// archives' own entry hashes; 106 entries, every one of them resolving:
//
//   set        women (Avatar_mgo)   men (Avatar)
//   base       1…28                 1…28        face preset portraits
//   deco       0…30                 0…30        0 = none
//   eyeblow    0…11                 0…11        0 = none
//   eye        1…8                  1…8         eye shapes
//   skincolor  1…8                  1…8         WRINKLE sets, not tones
//   hair       0…4                  0…3         0 = bald, then the styles
//   beard      —                    0…6         0 = clean-shaven (men only)
//   eyecolor   —                    1…8         irises, shared (see above)
//
// The two hair rows differ in length because the genders ship a different
// number of hair meshes — four for the women, three for the men — and both
// trees put bald in slot 0.
QString AvatarPresets::uiIconPath(const char* set, int n, Sex sex)
{
    if (sex == Sex::Men)
        return QStringLiteral("/Assets/tpp/ui/texture/Avatar/")
            + QLatin1String(set) + QStringLiteral("/ui_avatar_preset_")
            + QLatin1String(set) + QLatin1Char('_') + QString::number(n);
    return QStringLiteral("/Assets/ssd/ui/texture/Avatar_mgo/")
        + QLatin1String(set) + QStringLiteral("/ui_avatar_f_preset_")
        + QLatin1String(set) + QLatin1Char('_') + QString::number(n);
}

QString AvatarPresets::headStemFor(int faceType, Sex sex)
{
    if (faceType < 0) return {};
    return (sex == Sex::Men ? QStringLiteral("avm0_type%1_def")
                            : QStringLiteral("avf0_type%1_def"))
        .arg(faceType);
}

QString AvatarPresets::hairStemFor(int hairMesh, Sex sex)
{
    if (hairMesh < 0 || hairMesh > 25) return {};
    if (sex == Sex::Men)
        return QStringLiteral("avm_hair_") + QLatin1Char('a' + char(hairMesh))
            + QStringLiteral("0_v0_cov");
    // Concatenated, NOT arg(): "avf_hair_%10_v0_cov" makes Qt see placeholder
    // %10 rather than %1 followed by a zero, and quietly yields "avf_hair__v0_cov".
    return QStringLiteral("avf_hair_") + QLatin1Char('a' + char(hairMesh))
        + QStringLiteral("0_v0_cov");
}

QVector<AvatarPreset> AvatarPresets::parse(const QString& lua)
{
    const QVector<QVector<int>> base = tableOf(lua, "base");
    if (base.isEmpty()) return {};
    const QVector<QVector<int>> hair = tableOf(lua, "hair");
    const QVector<QVector<int>> skin = tableOf(lua, "skincolor");
    const QVector<QVector<int>> deco = tableOf(lua, "deco");
    const QVector<QVector<int>> eye = tableOf(lua, "eye");

    QVector<AvatarPreset> out;
    out.reserve(base.size());
    for (int i = 0; i < base.size(); ++i) {
        AvatarPreset p;
        p.index = i;
        p.faceType = optionOf(cell(base, i, 2));
        p.hairMesh = optionOf(cell(hair, i, 1));
        p.browShape = optionOf(cell(hair, i, 2));
        p.beard = optionOf(cell(hair, i, 3));
        p.beardVariant = qMax(0, optionOf(cell(hair, i, 4)));
        p.hairColour = optionOf(cell(hair, i, 5));
        p.skinColour = optionOf(cell(skin, i, 1));
        p.eyeSet = optionOf(cell(eye, i, 1));
        // The eye row's tail is two (iris, shade) pairs. Six morph sliders sit
        // between the lead code and them, so the pairs start at column 8.
        p.eyeColourR = optionOf(cell(eye, i, 8));
        p.eyeShadeR = qMax(0, optionOf(cell(eye, i, 9)));
        p.eyeColourL = optionOf(cell(eye, i, 10));
        p.eyeShadeL = qMax(0, optionOf(cell(eye, i, 11)));
        const int dt = cell(deco, i, 1);
        p.decoType = dt == 255 ? -1 : dt;
        p.decoId = p.decoType < 0 ? -1 : cell(deco, i, 2);
        p.decoSide = p.decoType < 0 ? 0 : cell(deco, i, 3);
        if (p.faceType < 0) continue;   // a row with no head is not a preset
        out.append(p);
    }
    return out;
}

const AvatarPresets& AvatarPresets::instance()
{
    static AvatarPresets c;
    const ArchiveIndex& index = ArchiveIndex::instance();
    // Same invalidation rule the other catalogues use: the files vector's
    // address changes when the index is rebuilt.
    static const void* key = nullptr;
    const void* now = index.files().constData();
    if (now != key) {
        key = now;
        c.build();
    }
    return c;
}

void AvatarPresets::build()
{
    m_presets.clear();
    m_presetsMen.clear();
    m_note.clear();
    const ArchiveIndex& index = ArchiveIndex::instance();

    // Dev hook: parse a decoded chunk from disk instead of the archives, so the
    // table can be exercised on an install that does not carry data1.dat.
    // Dev hook takes "<women.lua>" or "<women.lua>;<men.lua>" — filling only
    // the women's table would leave every male page falling back to raw head
    // models on a harness run, which is exactly the bug it is used to chase.
    const QByteArray override = qgetenv("FOXAB_AVATAR_LUA");
    if (!override.isEmpty()) {
        const QStringList paths = QString::fromLocal8Bit(override)
                                      .split(QLatin1Char(';'),
                                             Qt::SkipEmptyParts);
        for (int i = 0; i < paths.size() && i < 2; ++i) {
            QFile f(paths[i]);
            if (!f.open(QIODevice::ReadOnly)) continue;
            QVector<AvatarPreset> t = parse(QString::fromLatin1(f.readAll()));
            for (AvatarPreset& pr : t) pr.women = (i == 0);
            if (i == 0) m_presets = t; else m_presetsMen = t;
        }
        if (!m_presets.isEmpty() || !m_presetsMen.isEmpty()) {
            m_note = QStringLiteral("%1 women / %2 men preset(s) from "
                                    "FOXAB_AVATAR_LUA")
                         .arg(m_presets.size()).arg(m_presetsMen.size());
            qInfo("avatar: %s", qUtf8Printable(m_note));
            return;
        }
    }

    // Found by path suffix rather than by hashing a guessed name with a guessed
    // extension: the script's extension code is not one of the well-known ones,
    // and the index already carries the resolved path.
    // BOTH tables, told apart by the "_women" suffix. Taking the first path
    // that merely contains "avatar_presets" matched either file depending on
    // index order, which is how the men ended up with the women's faces.
    // The two tables are NOT both under /Assets/ssd/. Measured against the
    // shipped archives: the women's table is
    //   /Assets/ssd/ui/Script/avatar_presets_women.lua
    // and the men's is
    //   /Assets/tpp/ui/Script/avatar_presets.lua
    // — both inside Survive's own data1.dat, the men's simply named under the
    // tpp tree, exactly like the male avatar's head and hair models. Requiring
    // "/ssd/" therefore threw the men's table away, so ok(Men) was false, the
    // male survivor's Face Preset row fell back to raw model stems instead of
    // the game's numbered grid, and every appearance row that keys off a
    // preset index — eye colour, skin, wrinkles, brows, hair colour, feature —
    // never appeared at all.
    //
    // Neither TPP nor MGO ships a file at that path (checked across the staged
    // TPP + MGO archives: no avatar_presets under /Assets/tpp/ui/Script at
    // all), so accepting it cannot pull in another game's table. The "_women"
    // suffix still does the telling-apart, which is what actually matters:
    // matching on "avatar_presets" alone picked whichever came first in index
    // order, and that is how the men once ended up with the women's faces.
    int fileIdx = -1, fileIdxMen = -1;
    const auto& files = index.files();
    for (int i = 0; i < files.size(); ++i) {
        const QString& p = files[i].path;
        if (!p.contains(QLatin1String("/ui/Script/"), Qt::CaseInsensitive))
            continue;
        // The LAST SEGMENT, matched whole. A `contains("avatar_presets")` test
        // also claims avatar_presets_backup.lua and avatar_presets_dbg.lua —
        // and since the "_women" test runs over the same text, it would file
        // avatar_presets_women_backup.lua as the real women's table. With the
        // "/ssd/" root check gone this is the only thing keeping the match
        // honest, so it is an exact name rather than a substring.
        const QString leaf = p.section(QLatin1Char('/'), -1);
        const bool women =
            leaf.compare(QLatin1String("avatar_presets_women.lua"),
                         Qt::CaseInsensitive) == 0;
        const bool men = leaf.compare(QLatin1String("avatar_presets.lua"),
                                      Qt::CaseInsensitive) == 0;
        if (!women && !men) continue;
        // Prefer the copy the GAME would load: a replacement table in a
        // numbered master slot shadows the base-game one, and archives are
        // walked lowest-priority first, so taking the first match showed the
        // vanilla grid while the game ran the mod's.
        int& slot = women ? fileIdx : fileIdxMen;
        if (slot < 0 || (files[slot].shadowed && !files[i].shadowed)) slot = i;
    }
    if (fileIdxMen >= 0) {
        const QByteArray men = index.readFile(files[fileIdxMen]);
        if (!men.isEmpty()) {
            m_presetsMen = parse(QString::fromLatin1(men));
            for (AvatarPreset& pr : m_presetsMen) pr.women = false;
        }
    }
    // Every exit below reports BOTH tables and logs. The men's table is parsed
    // first, so a bare `return` here used to leave a note saying the face list
    // had fallen back to raw head models while ok(Men) was true and the male
    // page was showing the real grid — and it logged nothing at all.
    const auto finish = [&](const QString& womenNote) {
        m_note = womenNote;
        if (fileIdxMen >= 0)
            m_note += QStringLiteral("; %1 preset(s) for the men from %2")
                          .arg(m_presetsMen.size())
                          .arg(files[fileIdxMen].path.section(QLatin1Char('/'), -1));
        else
            m_note += QStringLiteral("; no men's table in the configured folders");
        qInfo("avatar: %s", qUtf8Printable(m_note));
    };
    if (fileIdx < 0) {
        finish(QStringLiteral(
            "avatar_presets_women is not in the configured folders — the "
            "women's face list falls back to the eight head models"));
        return;
    }
    const QByteArray blob = index.readFile(files[fileIdx]);
    if (blob.isEmpty()) {
        finish(QStringLiteral("%1 could not be read").arg(files[fileIdx].path));
        return;
    }
    m_presets = parse(QString::fromLatin1(blob));
    for (AvatarPreset& pr : m_presets) pr.women = true;
    if (m_presets.isEmpty()) {
        finish(QStringLiteral("%1 did not parse as a preset table")
                   .arg(files[fileIdx].path));
        return;
    }
    // How many distinct head models the presets actually use — the interesting
    // number, because it is not eight.
    QVector<int> seen;
    for (const AvatarPreset& p : m_presets)
        if (!seen.contains(p.faceType)) seen.append(p.faceType);
    // Both tables, always: reporting only the women's is how the men's being
    // rejected outright went unnoticed — the line looked healthy while half
    // the avatar system was missing.
    finish(QStringLiteral("%1 face preset(s) from %2, over %3 head model(s)")
               .arg(m_presets.size())
               .arg(files[fileIdx].path.section(QLatin1Char('/'), -1))
               .arg(seen.size()));
}

}  // namespace fox
