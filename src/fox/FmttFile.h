// FmttFile.h — Fox Engine material parameter table (".fmtt").
//
// The last piece of the material model. An FMDL material does not carry its own
// reflectance: it carries an INDEX, and this is the table that index reads.
//
// Layout, measured against the shipped file — 8192 bytes exactly, which is 256
// records of 32 bytes, matching the wiki's "there can only be 256 material
// presets assigned to this array". Each record is eight little-endian float32:
//
//   0x00  F0                        Fresnel reflectance at normal incidence
//   0x04  RoughnessThreshold
//   0x08  ReflectionDependDiffuse
//   0x0C  AnisotropicRoughness
//   0x10  SpecularColor.r
//   0x14  SpecularColor.g
//   0x18  SpecularColor.b
//   0x1C  Translucency
//
// The values settle the reading on their own. Preset 100 — the most common
// index in the shipped models — has F0 = 0.0392, which is the canonical
// dielectric 0.04 that every physically based renderer starts from. Preset 164
// is F0 0.886 with a specular colour of (0.80, 0.53, 0.16): gold. 165 is 0.945
// and white: silver. 3 is 0.871 and (1.00, 0.48, 0.28): copper. A layout that
// produces the real reflectance of gold at the index a gold-looking model
// points at is not a coincidence.
//
// Of the 256, 25 distinct F0 values are used, 24 of them above 0.5 (the
// metals), 10 presets carry a non-zero Translucency and 18 a non-zero
// AnisotropicRoughness.
//
// The table is really 100 presets, not 256. Rows 0..99 and 100..199 are
// BYTE-IDENTICAL — all 100 pairs, no exceptions — and rows 200..255 are one
// repeated filler row. Measured across the shipped models, no material indexes
// above 167 except the game's own 256 ("no material here", on 471 parameter
// rows), and 55 distinct indices are used in total. So the two banks are the
// same materials addressed twice and the tail is never reached.
//
// AnisotropicRoughness falls out along the same seam: the 18 non-zero rows are
// NINE identical pairs exactly 100 apart (29/129, 36/136, 38/138, 39/139,
// 45/145, 55/155, 62/162, 66/166, 67/167). The hair materials select 129 and
// 29 — F0 0.1608, four times the dielectric, with anisotropy 0.898 — which is
// where the hair highlight gets its strength from. Two more pairs are gold and
// silver at 0.898, which is brushed metal; this renderer has no path for that,
// so the value is consumed by the hair lobe only.
//
// RoughnessThreshold and ReflectionDependDiffuse are parsed and still unused.
// Neither has a reading the data settles: RoughnessThreshold is 0.0 on 142
// presets, 0.8 on 48, 0.498 on 24, then 0.6 / 0.298 / 0.4 / 1.0 / 0.847 in
// small counts; ReflectionDependDiffuse is non-zero on 40. Applying either on
// a guess would change every surface in the game.
//
// There is ONE table for the whole game, at
// /Assets/fox/effect/gr_pic/material_params.fmtt, with a near-identical
// platform variant under gr_pic/steam/ that differs in exactly two presets
// (164 and 165, in ReflectionDependDiffuse only).
#pragma once
#include <QByteArray>
#include <QVector>
#include <QString>
#include <cstdint>

namespace fox {

struct MaterialPreset {
    float f0 = 0.04f;
    float roughnessThreshold = 0.0f;
    float reflectionDependDiffuse = 0.0f;
    float anisotropicRoughness = 0.0f;
    float specular[3] = {1.0f, 1.0f, 1.0f};
    float translucency = 0.0f;
};

class FmttFile {
public:
    static bool isFmtt(const QByteArray& data);
    bool parse(const QByteArray& data);
    QString errorString() const { return m_error; }

    // Always 256 entries after a successful parse — a short file is padded
    // with the dielectric default rather than left ragged, so an index that
    // the table does not cover renders as ordinary plastic instead of black.
    const QVector<MaterialPreset>& presets() const { return m_presets; }
    // Bounds-checked lookup. 256 is the game's own "no material here" value on
    // MatParamIndex_1..3 and comes back as the dielectric default.
    MaterialPreset at(int index) const;
    // How many records the FILE held, before padding. presets() is always 256
    // after a successful parse; this is the honest count for a diagnostic.
    int readCount() const { return m_readCount; }

private:
    QVector<MaterialPreset> m_presets;
    int m_readCount = 0;
    QString m_error;
};

}  // namespace fox
