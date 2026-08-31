#include "gl/ViewEnvironment.h"

namespace fox {

// The presets. "Default" is the shader's own former constants, to the digit —
// check it against the kKey/kFill/kSky/kGround block in GLModelWidget.cpp
// before changing either. The rest are variations on the same rig rather than
// captured HDRIs: this viewport has no image-based lighting, and pretending
// otherwise with invented numbers would be worse than saying what they are.
//
// Each one moves three things together, because they only make sense together:
// what the key is (warm sun, cool overcast, hard studio), what the ambient
// hemisphere is standing in, and what the background is. A dark model on a
// dark background reads as nothing whichever light you put on it.
const QVector<ViewEnvironment>& ViewEnvironment::presets()
{
    static const QVector<ViewEnvironment> kAll = [] {
        QVector<ViewEnvironment> v;
        ViewEnvironment e;

        e = {};
        e.id = QStringLiteral("default");
        e.name = QStringLiteral("Default");
        e.note = QStringLiteral(
            "The viewport's own rig, unchanged — warm key, cool fill, a dim "
            "sky/ground hemisphere.");
        v << e;

        e = {};
        e.id = QStringLiteral("studio");
        e.name = QStringLiteral("Studio");
        e.note = QStringLiteral(
            "Neutral white key on a mid-grey surround. The one to judge a "
            "base colour in — nothing here tints the albedo.");
        e.key = {1.00f, 1.00f, 1.00f};
        e.fill = {0.42f, 0.44f, 0.48f};
        e.sky = {0.52f, 0.53f, 0.56f};
        e.ground = {0.30f, 0.30f, 0.31f};
        e.background = QColor(84, 86, 90);
        v << e;

        e = {};
        e.id = QStringLiteral("daylight");
        e.name = QStringLiteral("Daylight");
        e.note = QStringLiteral(
            "Hard warm sun against a blue sky and a sandy ground bounce — the "
            "strongest highlight of the set, so roughness reads hardest here.");
        e.key = {1.25f, 1.16f, 1.00f};
        e.fill = {0.24f, 0.32f, 0.48f};
        e.sky = {0.42f, 0.54f, 0.78f};
        e.ground = {0.32f, 0.27f, 0.19f};
        e.background = QColor(58, 72, 96);
        v << e;

        e = {};
        e.id = QStringLiteral("overcast");
        e.name = QStringLiteral("Overcast");
        e.note = QStringLiteral(
            "Almost no key, almost all hemisphere. Shows silhouette, ambient "
            "occlusion and the normal map's fine detail with nothing to blow "
            "them out.");
        e.key = {0.42f, 0.44f, 0.48f};
        e.fill = {0.34f, 0.36f, 0.40f};
        e.sky = {0.66f, 0.68f, 0.72f};
        e.ground = {0.34f, 0.34f, 0.35f};
        e.background = QColor(112, 115, 120);
        v << e;

        e = {};
        e.id = QStringLiteral("night");
        e.name = QStringLiteral("Night");
        e.note = QStringLiteral(
            "Cold, dim, near-black surround. What a reflective surface does "
            "with almost nothing to reflect — visors, optics, wet leather.");
        e.key = {0.42f, 0.48f, 0.62f};
        e.fill = {0.10f, 0.13f, 0.20f};
        e.sky = {0.10f, 0.12f, 0.18f};
        e.ground = {0.04f, 0.04f, 0.06f};
        e.background = QColor(10, 12, 17);
        e.exposure = 1.35f;
        v << e;

        e = {};
        e.id = QStringLiteral("showroom");
        e.name = QStringLiteral("Showroom");
        e.note = QStringLiteral(
            "Bright rim-heavy rig on black. For screenshots — it flatters, "
            "which also means it is the wrong one to judge a material in.");
        e.key = {1.30f, 1.26f, 1.18f};
        e.fill = {0.46f, 0.50f, 0.62f};
        e.sky = {0.58f, 0.60f, 0.68f};
        e.ground = {0.06f, 0.06f, 0.07f};
        e.background = QColor(0, 0, 0);
        v << e;

        e = {};
        e.id = QStringLiteral("flat");
        e.name = QStringLiteral("Flat white");
        e.note = QStringLiteral(
            "Even light from every side on white. Closest this viewport gets "
            "to showing the texture and nothing else.");
        e.key = {0.55f, 0.55f, 0.55f};
        e.fill = {0.55f, 0.55f, 0.55f};
        e.sky = {0.80f, 0.80f, 0.80f};
        e.ground = {0.72f, 0.72f, 0.72f};
        e.background = QColor(232, 232, 232);
        v << e;

        // ── THE FOUR GAMES ───────────────────────────────────────────────
        // One Fox install can hold four games whose art direction is not the
        // same, and the browser has been showing all of them under one rig.
        // These are AUTHORED, not read out of the games' own lighting data:
        // Fox keeps that in .grxla light arrays, .lpsh light probes and .atsh
        // sky harmonics, all of which live inside FPKs and none of which this
        // build reads yet (see --lightdump, which counts them). So they are a
        // deliberate reading of each game's look rather than a measurement,
        // and they are marked as such in the panel. When the light arrays are
        // decoded, these numbers are the thing to replace.
        e = {};
        e.id = QStringLiteral("game_tpp");
        e.name = QStringLiteral("TPP — Afghanistan noon");
        e.game = GameId::Tpp;
        e.note = QStringLiteral(
            "Authored, not measured. Hard high sun, dust-warmed bounce off "
            "pale rock, thin blue sky — the light most TPP character art was "
            "reviewed under.");
        e.key = {1.32f, 1.22f, 1.04f};
        e.fill = {0.26f, 0.30f, 0.40f};
        e.fillDir = {0.6f, 0.25f, 0.6f};
        e.sky = {0.40f, 0.50f, 0.70f};
        e.ground = {0.38f, 0.33f, 0.24f};
        e.background = QColor(62, 70, 86);
        v << e;

        e = {};
        e.id = QStringLiteral("game_gz");
        e.name = QStringLiteral("Ground Zeroes — rain at night");
        e.game = GameId::GroundZeroes;
        e.note = QStringLiteral(
            "Authored, not measured. One cold hard searchlight against a "
            "near-black surround, exposure lifted — GZ is a night mission and "
            "its materials were lit as wet.");
        e.key = {0.72f, 0.80f, 0.98f};
        e.fill = {0.08f, 0.11f, 0.17f};
        e.fillDir = {-0.5f, 0.15f, 0.6f};
        e.sky = {0.09f, 0.11f, 0.16f};
        e.ground = {0.04f, 0.05f, 0.07f};
        e.background = QColor(9, 11, 16);
        e.exposure = 1.30f;
        v << e;

        e = {};
        e.id = QStringLiteral("game_mgo");
        e.name = QStringLiteral("MGO3 — arena daylight");
        e.game = GameId::Mgo;
        e.note = QStringLiteral(
            "Authored, not measured. Flat, even and neutral: MGO's maps are "
            "lit so players read each other at range, so nothing here throws "
            "a silhouette into shadow.");
        e.key = {1.06f, 1.05f, 1.02f};
        e.fill = {0.44f, 0.46f, 0.50f};
        e.fillDir = {-0.6f, 0.3f, 0.5f};
        e.sky = {0.56f, 0.58f, 0.62f};
        e.ground = {0.34f, 0.33f, 0.32f};
        e.background = QColor(74, 78, 84);
        v << e;

        e = {};
        e.id = QStringLiteral("game_survive");
        e.name = QStringLiteral("Survive — dust overcast");
        e.game = GameId::Survive;
        e.note = QStringLiteral(
            "Authored, not measured. A sun behind haze: almost no direct key, "
            "a strong brown-grey hemisphere, everything a little desaturated "
            "— Survive's world is seen through the Dust.");
        e.key = {0.62f, 0.60f, 0.55f};
        e.fill = {0.34f, 0.33f, 0.31f};
        e.fillDir = {0.4f, 0.3f, 0.6f};
        e.sky = {0.60f, 0.57f, 0.50f};
        e.ground = {0.32f, 0.29f, 0.24f};
        e.background = QColor(96, 92, 84);
        v << e;

        return v;
    }();
    return kAll;
}

const ViewEnvironment* ViewEnvironment::find(const QString& id)
{
    for (const ViewEnvironment& e : presets())
        if (e.id == id) return &e;
    return nullptr;
}

const ViewEnvironment* ViewEnvironment::forGame(GameId g)
{
    if (g == GameId::Unknown) return nullptr;
    for (const ViewEnvironment& e : presets())
        if (e.game == g) return &e;
    return nullptr;
}

const char* debugViewName(DebugView v)
{
    switch (v) {
        case DebugView::Off:            return "Shaded";
        case DebugView::Albedo:         return "Albedo";
        case DebugView::Normal:         return "Normals";
        case DebugView::Roughness:      return "Roughness";
        case DebugView::ReflectionMask: return "Reflection mask";
        case DebugView::Occlusion:      return "Ambient occlusion";
        case DebugView::Translucency:   return "Translucency";
        case DebugView::Metalness:      return "Metalness";
        case DebugView::Uv:             return "UV";
        case DebugView::LightingOnly:   return "Lighting only";
    }
    return "Shaded";
}

// What each view is FOR, and — where it matters — where the number comes from,
// because three of these are channels of one texture and two are derived
// rather than sampled.
const char* debugViewNote(DebugView v)
{
    switch (v) {
        case DebugView::Off:
            return "The lit result.";
        case DebugView::Albedo:
            return "Base colour after the layer/dye composite, unlit.";
        case DebugView::Normal:
            return "World-space shading normal as RGB, normal map applied. "
                   "World space, not tangent space: a mesh with no normal map "
                   "still shows the full spectrum, and what the map adds is "
                   "the fine detail on top of that.";
        case DebugView::Roughness:
            return "SRM green. Black is a mirror, white is matte.";
        case DebugView::ReflectionMask:
            return "SRM blue — the reflectivity boost. 61% of texels in the "
                   "shipped data are exactly 0, so mostly black is correct.";
        case DebugView::Occlusion:
            return "SRM red.";
        case DebugView::Translucency:
            return "TRM, or the material preset's own value where no TRM "
                   "ships. Skin is floored at 0.25.";
        case DebugView::Metalness:
            return "Not a shipped map: derived from the FMTT preset's F0, and "
                   "forced to 0 for skin, hair, cloth and eyes.";
        case DebugView::Uv:
            return "fract(uv) as red/green — seams, wrapping and mirrored "
                   "islands.";
        case DebugView::LightingOnly:
            return "The full rig on a flat grey albedo: the lighting with the "
                   "textures taken away.";
    }
    return "";
}

QVector<DebugView> debugViews()
{
    return {DebugView::Off,           DebugView::Albedo,
            DebugView::Normal,        DebugView::Roughness,
            DebugView::ReflectionMask, DebugView::Occlusion,
            DebugView::Translucency,  DebugView::Metalness,
            DebugView::Uv,            DebugView::LightingOnly};
}

}  // namespace fox
