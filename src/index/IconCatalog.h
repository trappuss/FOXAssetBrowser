// IconCatalog.h — the game's own UI icons, decoded on demand.
//
// MGSV draws every customization part as a white line-art silhouette on
// transparency: /Assets/tpp/ui/texture/WeaponPartsIcon/<slot>/ui_wpp_<stem>_alp
// for weapon parts, /Assets/tpp/ui/texture/EquipIcon/… for the rest. The path
// is not guessed — WeaponPartsUiSetting.lua names it per part, and NameCatalog
// already reads it while collecting the display names.
//
// The art sits inside a 256x128 canvas with a lot of transparent margin, so
// every icon is trimmed to its alpha bounding box before it is scaled: without
// that, a magazine ends up a third the height of a receiver in the same list.
//
// Decoding is lazy and cached by (stem, height). Assembling a .ftex plus its
// .ftexs streams is far too slow to do while painting a list, so a miss paints
// nothing and the row simply has no icon.
#pragma once
#include <QHash>
#include <QPixmap>
#include <QString>

namespace fox {

class IconCatalog {
public:
    static IconCatalog& instance();

    // The part's icon, trimmed and scaled to `height` pixels. Null when the
    // part has no icon, when the texture is not in the configured folders, or
    // when it cannot be decoded — every caller must cope with a null pixmap.
    QPixmap iconFor(const QString& modelStem, int height);

    // A UI texture addressed by its own asset path (no extension), scaled to
    // `size` square and NOT alpha-trimmed — colour swatches fill their canvas,
    // and trimming one would crop the art rather than centre it. This is the
    // path the customize screen's colour icons live on
    // (/Assets/tpp/ui/texture/Customize/color/…), which no part stem names.
    QPixmap swatchForPath(const QString& assetPathNoExt, int size);
    // Build the "crop:"/"avg:"/"alpha:" spec strings swatchForPath understands
    // for a choice that ships no icon of its own. See IconCatalog.cpp.
    static QString cropSpec(const QString& path, qreal x, qreal y, qreal w,
                            qreal h);
    static QString avgSpec(const QString& path, qreal x, qreal y, qreal w,
                           qreal h);
    static QString alphaSpec(const QString& path);

    // The red crossed box the customize screen draws where a weapon cannot
    // take a part at all. Generated rather than extracted: the game's own is a
    // 9-slice UI sprite, and one drawn to the row's height scales cleanly.
    QPixmap crossedOut(int height);

    // Drop everything: the archives changed, so the file indices behind the
    // cached pixmaps are meaningless.
    void reset();

    int cachedCount() const { return m_cache.size(); }

    // Is this part's icon WHITE LINE ART, as the weapon-parts icons are? Those
    // are drawn in game over a dark panel and have to be recoloured to stay
    // visible under a light theme. The suit icons are not: they are full-colour
    // photographs of the outfit, and recolouring one turns it into a solid
    // block. Answered from the same cache the pixmap comes from, so a caller
    // painting a row pays a hash lookup and no pixel work.
    //
    // Only meaningful after iconFor() has been called for the same stem; an
    // unknown stem answers true, which is the pre-existing behaviour.
    bool iconIsLineArt(const QString& modelStem) const;

private:
    QPixmap decode(const QString& modelStem, int height);
    QPixmap decodePath(const QString& assetPathNoExt, int size);
    QPixmap derivedSwatch(const QString& spec, int size);
    static QImage decodeImage(const QString& assetPathNoExt);

    QHash<QString, QPixmap> m_cache;   // "<stem>@<height>" → pixmap (may be null)
    QHash<QString, bool> m_lineArt;    // stem → its icon is white line art
};

}  // namespace fox
