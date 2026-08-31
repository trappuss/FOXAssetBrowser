// LayerColors.h — the game's own colour palette, read out of the archives.
//
// A piece of colour-customizable gear does not ship coloured. Its base map is
// white or grey and its material runs a "LayerMul" shader, which multiplies a
// separate flat COLOUR SWATCH through a greyscale LAYER MASK painted in the
// model's own UV layout. Selecting a colour in game rebinds one texture role
// (Layer_Tex_SRGB) to a different swatch — nothing else changes, no model is
// reloaded, no material is rewritten.
//
// Measured, not assumed:
//   • 6,419 of the 14,516 texture substitutions in the 1,895 shipped .fv2
//     tables target Layer_Tex_SRGB — by a wide margin the most-substituted
//     role in the game. Colour IS the layer slot.
//   • 315 of the 412 Layer_Tex references baked into shipped models point at
//     /Assets/tpp/common_source/flat/Pictures/cm_flat_white — the placeholder
//     the game replaces at runtime. That white is exactly why an unmodified
//     export of a customizable garment looks bleached.
//   • The swatches live in /Assets/tpp/common_source/layer/ and there are 125
//     of them: cm_scol3 (22) and cm_scol4 (25) are SOLIDS — every texel of the
//     512x512 image is one colour — and cm_camo3 (18) and cm_camo4 (60) are
//     patterns.
//
// This catalogue is a directory scan and nothing more; it does no decoding
// until someone asks for a colour chip, because 47 BC7 decodes to draw a combo
// box that may never be opened is not a cost worth paying at index time.
#pragma once
#include <QColor>
#include <QHash>
#include <QString>
#include <QVector>
#include <cstdint>

namespace fox {

struct LayerSwatch {
    QString id;          // "c00" — the game's own index within its family
    QString family;      // "cm_scol4", "cm_camo3", …
    QString path;        // full asset path, extension included
    QString basePath;    // the same without ".ftex" — what an icon helper wants
    quint64 pathHash = 0;  // PathCode64, ready to drop into a FOVA override
    bool solid = false;  // a "scol" family: one flat colour, not a pattern
    bool shadowed = false;  // another mounted archive supersedes this copy
};

class LayerColorCatalog {
public:
    static LayerColorCatalog& instance();

    // Scan the archive index. Safe to call more than once; a second call after
    // a rescan rebuilds. Cheap — it touches no texture data.
    void build();
    bool ok() const { return !m_solids.isEmpty() || !m_patterns.isEmpty(); }
    QString note() const { return m_note; }

    // Sorted by family then index, which is the order the customize screen
    // shows them in.
    const QVector<LayerSwatch>& solids() const { return m_solids; }
    const QVector<LayerSwatch>& patterns() const { return m_patterns; }

    // The representative colour of a swatch, decoded on FIRST request and
    // cached. Meaningful for the solid families; for a pattern it is the
    // average, which is what a small chip would look like anyway. Returns an
    // invalid QColor when the texture cannot be decoded (and does not cache
    // that, so a transient read failure is retried).
    //
    // GUI THREAD ONLY. It is const, but the cache behind it is `mutable` and
    // unlocked; two threads inserting into the same QHash corrupt it.
    QColor colorOf(const LayerSwatch& s) const;

private:
    QVector<LayerSwatch> m_solids;
    QVector<LayerSwatch> m_patterns;
    mutable QHash<quint64, QColor> m_colorCache;
    QString m_note;
};

}  // namespace fox
