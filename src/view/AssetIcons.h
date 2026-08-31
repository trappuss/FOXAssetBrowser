// AssetIcons.h — one icon vocabulary for Fox assets, shared by every view.
//
// The Files tab had no icons at all: a tree of forty thousand rows all drawn
// the same, where the only way to tell a model from a texture from a script
// was to read the extension at the end of the name. The Models outliner grew
// its own icons batch by batch. Both of those are the same question — "what
// kind of thing is this row" — and it should have one answer.
//
// TWO AXES, DELIBERATELY:
//
//   • The GLYPH says what KIND of asset it is. A model, a texture, an
//     animation archive, a rig, a script, a container, audio.
//   • The COLOUR says which GAME it belongs to for FOLDERS (red TPP, white
//     MGO, blue Ground Zeroes, yellow Survive — the user's scheme), and which
//     KIND it is for FILES.
//
// Colouring files by game as well was tried on paper and rejected: in a tree
// where every row under /Assets/tpp is red, the colour carries no information
// the folder above it did not already give, and it costs the one channel that
// could have said "these are the textures". Folders answer where you are;
// files answer what you are looking at.
//
// EVERYTHING IS CACHED BY (glyph, colour, size). foxglyph runs a QPainter and
// builds a 1x and a 2x pixmap per call, and a model that calls this from
// data() is calling it once per row per repaint — the outliner cost 473 ms of
// a 501 ms build learning that lesson once already.
#pragma once
#include <QColor>
#include <QHash>
#include <QIcon>
#include <QString>

#include "index/GameId.h"
#include "view/ViewGlyphs.h"

namespace fox {
namespace asseticon {

// The kinds this vocabulary distinguishes. Anything unrecognised is Other and
// draws a plain page — never a guess at a nearby kind.
enum class Kind {
    Other, Model, Texture, Animation, Rig, Connect, Variation, Material,
    Script, Text, Audio, Container, Ui,
};

inline Kind kindFor(const QString& ext)
{
    const QString e = ext.toLower();
    if (e == QLatin1String("fmdl")) return Kind::Model;
    if (e == QLatin1String("ftex") || e == QLatin1String("ftexs")
        || e == QLatin1String("dds") || e == QLatin1String("pftxs"))
        return Kind::Texture;
    if (e == QLatin1String("mtar") || e == QLatin1String("gani"))
        return Kind::Animation;
    if (e == QLatin1String("frig") || e == QLatin1String("frdv")
        || e == QLatin1String("fdrm") || e == QLatin1String("dfrm"))
        return Kind::Rig;
    if (e == QLatin1String("fcnp")) return Kind::Connect;
    if (e == QLatin1String("fv2") || e == QLatin1String("fova"))
        return Kind::Variation;
    if (e == QLatin1String("fmtt") || e == QLatin1String("fmtl"))
        return Kind::Material;
    if (e == QLatin1String("lua")) return Kind::Script;
    if (e == QLatin1String("lng2") || e == QLatin1String("subp")
        || e == QLatin1String("xml") || e == QLatin1String("txt"))
        return Kind::Text;
    if (e == QLatin1String("wem") || e == QLatin1String("bnk")
        || e == QLatin1String("wav"))
        return Kind::Audio;
    if (e == QLatin1String("qar") || e == QLatin1String("fpk")
        || e == QLatin1String("fpkd") || e == QLatin1String("dat")
        || e == QLatin1String("g0s") || e == QLatin1String("pftxs"))
        return Kind::Container;
    if (e == QLatin1String("uilb") || e == QLatin1String("uigb"))
        return Kind::Ui;
    return Kind::Other;
}

// Muted, not saturated: these are line glyphs a dozen pixels across sitting
// beside text, and a column of primaries reads as a toy rather than as a
// classification.
inline QColor inkFor(Kind k)
{
    switch (k) {
        case Kind::Model:     return QColor(0xd8, 0xa8, 0x5c);   // amber
        case Kind::Texture:   return QColor(0xb0, 0x84, 0xd0);   // violet
        case Kind::Animation: return QColor(0x6f, 0xb8, 0x8a);   // green
        case Kind::Rig:       return QColor(0x6f, 0xb4, 0xc8);   // cyan
        case Kind::Connect:   return QColor(0x88, 0xa8, 0xd8);   // pale blue
        case Kind::Variation: return QColor(0xd0, 0x92, 0xb0);   // rose
        case Kind::Material:  return QColor(0xc0, 0xb0, 0x78);   // olive
        case Kind::Script:    return QColor(0x9a, 0xc0, 0x70);   // leaf
        case Kind::Text:      return QColor(0x9a, 0x9a, 0xa4);   // grey
        case Kind::Audio:     return QColor(0xd8, 0x8a, 0x9a);   // pink
        case Kind::Container: return QColor(0xb8, 0x9c, 0x7c);   // tan
        case Kind::Ui:        return QColor(0x7c, 0xa8, 0xb8);   // steel
        default:              return QColor();                   // palette ink
    }
}

inline int glyphFor(Kind k)
{
    switch (k) {
        case Kind::Model:     return 13;   // a boxed-up part
        case Kind::Texture:   return 21;   // a picture
        case Kind::Animation: return 22;   // a key on a track
        case Kind::Rig:       return 1;    // two bones from a joint
        case Kind::Connect:   return 15;   // a body with a piece clipped on
        case Kind::Variation: return 5;    // a swatch stack
        case Kind::Material:  return 5;
        case Kind::Script:    return 26;   // a page with code
        case Kind::Text:      return 26;
        case Kind::Audio:     return 28;   // a speaker
        case Kind::Container: return 27;   // a crate
        case Kind::Ui:        return 17;   // rows and tiles
        default:              return 25;   // a plain page
    }
}

// The FOLDER colours, by game. Same scheme the Models outliner uses, so a
// folder means the same thing in both tabs.
inline QColor folderInk(GameId g)
{
    switch (g) {
        case GameId::Tpp:          return QColor(0xd0, 0x5a, 0x5a);   // red
        case GameId::Mgo:          return QColor(0xe8, 0xe8, 0xee);   // white
        case GameId::GroundZeroes: return QColor(0x6f, 0x9f, 0xd8);   // blue
        case GameId::Survive:      return QColor(0xd8, 0xb4, 0x5a);   // yellow
        default:                   return QColor();                   // ink
    }
}

namespace detail {
inline QHash<QString, QIcon>& cache()
{
    static QHash<QString, QIcon> c;
    return c;
}
inline QIcon get(int glyph, const QColor& ink, int px, qreal frac)
{
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(glyph)
                            .arg(ink.isValid() ? ink.name() : QStringLiteral("-"))
                            .arg(px)
                            .arg(int(frac * 100));
    auto& c = cache();
    const auto it = c.constFind(key);
    if (it != c.constEnd()) return *it;
    const QIcon ic = frac >= 0.999 ? foxglyph::toolIconAt(glyph, px, ink)
                                   : foxglyph::toolIconInset(glyph, px, frac, ink);
    c.insert(key, ic);
    return ic;
}
}  // namespace detail

// A folder, coloured by the game whose assets are under it.
inline QIcon folder(GameId g, int px)
{
    return detail::get(19, folderInk(g), px, 0.72);
}

// A file, by extension. `px` is the icon box; the glyph is drawn slightly
// inset so a column of them does not crowd the text beside it.
inline QIcon file(const QString& ext, int px)
{
    const Kind k = kindFor(ext);
    return detail::get(glyphFor(k), inkFor(k), px, 0.78);
}

// What to call this kind in a tooltip or a column.
inline QString kindName(const QString& ext)
{
    switch (kindFor(ext)) {
        case Kind::Model:     return QStringLiteral("model");
        case Kind::Texture:   return QStringLiteral("texture");
        case Kind::Animation: return QStringLiteral("animation");
        case Kind::Rig:       return QStringLiteral("rig");
        case Kind::Connect:   return QStringLiteral("connect points");
        case Kind::Variation: return QStringLiteral("variation table");
        case Kind::Material:  return QStringLiteral("material table");
        case Kind::Script:    return QStringLiteral("script");
        case Kind::Text:      return QStringLiteral("text");
        case Kind::Audio:     return QStringLiteral("audio");
        case Kind::Container: return QStringLiteral("container");
        case Kind::Ui:        return QStringLiteral("UI layout");
        default:              return QString();
    }
}

}  // namespace asseticon
}  // namespace fox
