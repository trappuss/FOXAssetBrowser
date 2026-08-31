// AvatarPresets.h — Metal Gear Survive's own avatar face-preset table.
//
// The AVATAR screen's "Basic Face Shape" grid is not a list of head models. It
// is 28 numbered presets, and the game keeps their definition in one Lua table
// it ships in the archives:
//
//   /Assets/ssd/ui/Script/avatar_presets_women   (data1.dat)
//
// which decodes to
//
//   local e={}e.presets={eye={{…}},nose={{…}},mouth={{…}},chin={{…}},
//                        cheek={{…}},head={{…}},hair={{…}},skincolor={{…}},
//                        deco={{…}},base={{…}}}return e
//
// Ten tables of 28 rows each. `base` is the master: row N is
//
//   {N, 128, faceCode, N, N, N, N, N, N, N, N, N}
//
// — the trailing nine are just N, i.e. preset N takes row N of every other
// table, and `faceCode` picks the HEAD MODEL: 128+k is avf0_type<k>_def. Seven
// of the eight shipped heads are used (type 5 by no preset at all).
//
// Everything else in a preset is a texture or a slider, not a model:
//
//   hair[N]      = {N, meshCode, browShape, beard, beardVar, colourCode}
//                  meshCode 128+k = avf_hair_{a,b,c,d}0_v0_cov, 255 = bald
//   skincolor[N] = {N, skinCode, faceCode}   — the face code repeats base's
//   deco[N]      = {N, decoType, decoId, side}      decoType 255 = no feature
//   eye/nose/mouth/chin/cheek/head[N] = a lead code and then 0-10 morph sliders
//
// The hair row's middle fields are PROVEN, not guessed, by diffing this file
// against its male twin /Assets/ssd/ui/Script/avatar_presets (same ten tables,
// same 28 rows, same column count). The women's file offsets mesh, skin, face
// and colour codes by 128; the men's file writes them raw. The two columns that
// are NOT offset in either file are columns 2 and 3:
//
//              women            men
//   col 2      4…9              0…10      → eyebrow shape, 11 shapes a0…k0
//   col 3      255 (all rows)   0…4, 255  → beard, 255 = none (men only)
//   col 4      0   (all rows)   0…2       → beard variant
//
// A field that is always 255 for every woman and 0-4 for men is a beard; a
// field that spans the full 0-10 brow range in the men's file is the brow. The
// women's 4-9 is simply the subset the 28 women's presets happen to use.
// Decoding rule for both files: v == 255 → none, v >= 128 → v - 128, else v.
//
// The morph sliders drive the .dfrm deformation the browser does not implement,
// so a preset here reproduces the head MODEL, its hair and the rest of the
// preset's definition — not the sculpted shape. That limit is stated in the UI
// rather than hidden.
//
// The numbers are read from the shipped file, never hard-coded: a different
// build of the game ships a different table and this follows it.
#pragma once
#include <QString>
#include <QVector>

namespace fox {

struct AvatarPreset {
    int index = 0;        // 0-based; the game's grid numbers them from 1
    // Which table this row came out of. Only describe() needs it — the head
    // stem it names is avf0_type<N> for the women and avm0_type<N> for the men,
    // and hard-coding "avf0" put "head avf0_type3" under every row of the MEN'S
    // grid, beside a path that plainly said avm0.
    bool women = true;
    int faceType = -1;    // av[fm]0_type<faceType>_def
    int hairMesh = -1;    // 0-3 → avf_hair_{a,b,c,d}0_v0_cov; -1 = bald
    int hairColour = -1;  // 0-4
    // hair[N][2] — the eyebrow shape, 0-based into browShapes() (11 shapes
    // a0…k0). Proven against the male preset table, which spans 0-10 in this
    // column; see the header comment.
    int browShape = -1;
    // hair[N][3] and [4] — beard and beard variant. Always 255/0 for a woman.
    int beard = -1;
    int beardVariant = 0;
    int eyeSet = -1;      // eye texture set
    // eye[N] columns 8-11: two (iris index, shade) pairs, right eye then left.
    // Two pairs and not one repeated value — the men's table has rows where
    // they differ (preset 17 is one copper eye and one white one), which is
    // what proves the column is per-eye. Indices run 0-7 over the eight shipped
    // cm_iris maps; the shade flag is 0 bright / 1 dark.
    int eyeColourR = -1;
    int eyeColourL = -1;
    int eyeShadeR = 0;
    int eyeShadeL = 0;
    int skinColour = -1;  // 0-4
    int decoType = -1;    // -1 = no facial feature
    int decoId = -1;
    int decoSide = 0;
    // One line describing everything the preset sets, for the row subtitle.
    QString describe() const;
};

class AvatarPresets {
public:
    // Which of the two shipped tables. Survive carries BOTH — avatar_presets
    // for the men and avatar_presets_women for the women — with identical
    // schemas and 28 rows each. Reading whichever the index happened to list
    // first gave one gender the other's faces.
    enum class Sex { Women, Men };

    static const AvatarPresets& instance();

    bool ok() const { return !m_presets.isEmpty(); }
    // The women's table, kept as the default so existing callers are unchanged.
    const QVector<AvatarPreset>& presets() const { return m_presets; }
    const QVector<AvatarPreset>& presets(Sex sex) const
    {
        return sex == Sex::Men ? m_presetsMen : m_presets;
    }
    bool ok(Sex sex) const { return !presets(sex).isEmpty(); }
    // Which table a head model belongs to, from its stem (avm0_type3_def).
    static Sex sexOfStem(const QString& stem)
    {
        return stem.startsWith(QLatin1String("avm")) ? Sex::Men : Sex::Women;
    }
    // Where the table was read from, or why it could not be.
    QString note() const { return m_note; }

    // The game's own thumbnail for preset `index` (0-based), as an asset path
    // with no extension — the same art the AVATAR screen draws in its grid.
    static QString iconPathFor(int index, Sex sex = Sex::Women);
    // The game's own thumbnail from one of the AVATAR screen's icon sets. There
    // are TWO complete trees, one per gender — see the note in
    // AvatarPresets.cpp — and `sex` picks which. `n` is the shipped file's own
    // number.
    static QString uiIconPath(const char* set, int n, Sex sex = Sex::Women);
    // Icon for one option, with the set's own numbering applied. See the
    // numbering table in AvatarPresets.cpp — the sets do not agree on whether
    // they are 0- or 1-based, and deco reserves a fixed block per family.
    static QString decoIconPath(int family, int id, Sex sex = Sex::Women);
    // -1 = the "none" slot.
    static QString browIconPath(int shapeIdx, Sex sex = Sex::Women);
    // The AVATAR/EYES screen's own tile for eye shape N (0-based here, the
    // shipped files are numbered from 1).
    static QString eyeIconPath(int shapeIdx, Sex sex = Sex::Women);
    // The AVATAR/SKIN screen's own tile for wrinkle set N (0-based here). Eight
    // tiles ship per gender; a set beyond them has none.
    static QString wrinkleIconPath(int setIdx, Sex sex = Sex::Women);
    // Iris colour tile, 0-based index into the eight shipped colours. One set
    // serves both genders — an iris has no gender — see the definition.
    static QString eyeColourIconPath(int colourIdx);
    // -1 = bald, which is tile 0 in both trees.
    static QString hairIconPath(int meshIdx, Sex sex = Sex::Women);
    // The men's AVATAR screen has a Beard row and its own tiles for it.
    // `family` is 0-4 for the five shipped beard families, or -1 for the
    // clean-shaven tile. The women's tree has no such set.
    static QString beardIconPath(int family);
    // The head model stem a face code selects.
    static QString headStemFor(int faceType, Sex sex = Sex::Women);
    // The hair model stem a hair code selects, or empty for bald.
    static QString hairStemFor(int hairMesh, Sex sex = Sex::Women);

    // Parse a decoded avatar_presets Lua chunk (exposed for tests/probes).
    static QVector<AvatarPreset> parse(const QString& lua);

private:
    AvatarPresets() = default;
    void build();

    QVector<AvatarPreset> m_presets;      // women
    QVector<AvatarPreset> m_presetsMen;
    QString m_note;
};

}  // namespace fox
